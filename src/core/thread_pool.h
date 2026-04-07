#pragma once
// Work-stealing thread pool.
// Root tasks are submitted using round-robin to a per-worker MPSC queue. 
// Subtasks can be submitted directly locally using `submit_local()` to the owner's Deque.

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

        // Push to a worker's submission queue using round-robin.
        auto idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
        submission_queues_[idx]->push(task);

        // Wake a sleeping worker
        wake_one();

        return future;
    }

    /// Submit a callable locally to the calling worker's private deque.
    /// MUST be called from within a thread pool worker thread.
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
    void wake_one();

    unsigned num_threads_;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> next_worker_{0};

    // Per-worker submission queues for root tasks (FIFO).
    std::vector<std::unique_ptr<MPSCQueue<Task>>> submission_queues_;

    // Per-worker deques for nested subtasks (LIFO local, FIFO steal).
    std::vector<std::unique_ptr<WorkStealingDeque<Task>>> deques_;
    std::vector<std::thread> threads_;

    // Sleeping / waking
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::atomic<unsigned> sleeping_count_{0};
};

}  // namespace forge
