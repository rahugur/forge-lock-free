#pragma once
// Work-stealing thread pool.
// Root tasks are submitted using round-robin to a per-worker MPSC queue.
// Subtasks can be submitted directly locally using `submit_local()` to the owner's Deque.
//
// The submit → wake path is lock-free: wake uses an atomic epoch bump,
// workers spin-check the epoch with backoff before sleeping.

#include <atomic>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include <cassert>

#include "core/future.h"
#include "core/mpsc_queue.h"
#include "core/work_stealing_deque.h"

namespace forge {

extern thread_local int t_worker_id;

class ThreadPool {
public:
    /// Construct a pool with `num_threads` workers.
    /// If num_threads == 0, uses std::thread::hardware_concurrency().
    explicit ThreadPool(unsigned num_threads = 0);

    /// Shutdown: signals all workers and joins.
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Submit a callable globally and receive a Future for the result.
    /// The submit path is lock-free.
    template <typename F>
    auto submit(F&& func) -> Future<decltype(func())> {
        using R = decltype(func());

        auto promise = std::make_shared<Promise<R>>();
        auto future = promise->get_future();

        auto task = new std::function<void()>(
            [p = std::move(promise), f = std::forward<F>(func)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        f();
                        p->set_value();
                    } else {
                        p->set_value(f());
                    }
                } catch (...) {
                    try {
                        p->set_exception(std::current_exception());
                    } catch (...) {}
                }
            });

        if (t_worker_id >= 0 && t_worker_id < static_cast<int>(num_threads_)) {
            // Called from a worker thread — push to local deque.
            // Other idle workers can steal it, avoiding deadlock when
            // the submitter later blocks on the returned future.
            deques_[t_worker_id]->push(task);
        } else {
            // Called from an external thread — push to a submission queue.
            auto idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
            submission_queues_[idx]->push(task);
        }

        // Wake a sleeping worker (lock-free).
        notify();

        return future;
    }

    /// Submit a callable locally to the calling worker's private deque.
    /// MUST be called from within a thread pool worker thread.
    ///
    /// WARNING: if the caller blocks on the returned future (via .get()),
    /// the subtask must be stolen by another worker.  With num_threads==1,
    /// or when all workers are similarly blocked, this is a deadlock.
    /// Prefer submit() when the caller will block on the result.
    template <typename F>
    auto submit_local(F&& func) -> Future<decltype(func())> {
        using R = decltype(func());
        auto promise = std::make_shared<Promise<R>>();
        auto future = promise->get_future();

        auto task = new std::function<void()>(
            [p = std::move(promise), f = std::forward<F>(func)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        f();
                        p->set_value();
                    } else {
                        p->set_value(f());
                    }
                } catch (...) {
                    try {
                        p->set_exception(std::current_exception());
                    } catch (...) {}
                }
            });

        assert(t_worker_id >= 0 && t_worker_id < static_cast<int>(num_threads_));
        deques_[t_worker_id]->push(task);

        return future;
    }

    /// Number of worker threads.
    unsigned num_threads() const { return num_threads_; }

private:
    using Task = std::function<void()>*;

    void worker_loop(unsigned id);

    /// Try to process one task from the given worker's perspective.
    /// Returns true if a task was found and executed.
    /// Used by Future::get() to avoid pool starvation.
    bool try_process_one(unsigned id);

    /// Bump the epoch to wake sleeping workers (lock-free).
    void notify();

    unsigned num_threads_;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> next_worker_{0};

    // Per-worker submission queues for root tasks (FIFO).
    std::vector<std::unique_ptr<MPSCQueue<Task>>> submission_queues_;

    // Per-worker deques for nested subtasks (LIFO local, FIFO steal).
    std::vector<std::unique_ptr<WorkStealingDeque<Task>>> deques_;
    std::vector<std::thread> threads_;

    // Lock-free sleep/wake: workers spin on epoch_, falling back to
    // short sleeps.  notify() bumps the epoch; workers detect the
    // change and re-poll their queues.
    alignas(64) std::atomic<uint64_t> epoch_{0};
};

}  // namespace forge
