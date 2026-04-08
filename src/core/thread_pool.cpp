#include "core/thread_pool.h"

#include <spdlog/spdlog.h>

#include <random>

namespace forge {

thread_local int t_worker_id = -1;
thread_local ThreadPool* t_pool = nullptr;

ThreadPool::ThreadPool(unsigned num_threads)
    : num_threads_(num_threads == 0
                       ? std::max(1u, std::thread::hardware_concurrency())
                       : num_threads) {
    deques_.reserve(num_threads_);
    submission_queues_.reserve(num_threads_);
    for (unsigned i = 0; i < num_threads_; ++i) {
        deques_.push_back(std::make_unique<WorkStealingDeque<Task>>());
        submission_queues_.push_back(std::make_unique<MPSCQueue<Task>>());
    }

    threads_.reserve(num_threads_);
    for (unsigned i = 0; i < num_threads_; ++i) {
        threads_.emplace_back(&ThreadPool::worker_loop, this, i);
    }

    spdlog::debug("ThreadPool started with {} workers", num_threads_);
}

ThreadPool::~ThreadPool() {
    stop_.store(true, std::memory_order_release);
    // Bump epoch to wake all sleeping workers.
    epoch_.fetch_add(1, std::memory_order_release);

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    // All workers have exited.  Single-threaded final drain of any
    // remaining tasks (shouldn't happen normally, but prevents leaks).
    for (auto& dq : deques_) {
        while (auto t = dq->pop()) {
            (*(*t))();
            delete *t;
        }
    }
    for (auto& sq : submission_queues_) {
        while (auto task = sq->pop()) {
            (*(*task))();
            delete *task;
        }
    }
    spdlog::debug("ThreadPool shut down");
}

void ThreadPool::worker_loop(unsigned id) {
    t_worker_id = id;

    // Install yield callback so Future::get() can process tasks while waiting.
    t_pool = this;
    detail::yield_fn = []() -> bool {
        return t_pool->try_process_one(t_worker_id);
    };

    // Thread-local RNG for picking steal targets
    std::mt19937 rng(id * 31 + 7);
    std::uniform_int_distribution<unsigned> dist(0, num_threads_ - 1);
    unsigned tick = 0;

    while (true) {
        Task task_ptr = nullptr;

        // 0. Periodically poll submission queue to prevent root-task starvation
        if (++tick % 64 == 0) {
            if (auto s = submission_queues_[id]->pop()) {
                task_ptr = *s;
            }
        }

        // 1. Try own deque first (LIFO — optimized for subtasks)
        if (!task_ptr) {
            if (auto t = deques_[id]->pop()) {
                task_ptr = *t;
            }
        }

        // 2. Try submission queue (FIFO — optimized for root tasks)
        if (!task_ptr) {
            if (auto s = submission_queues_[id]->pop()) {
                task_ptr = *s;
            }
        }

        // 3. Steal from a random peer
        if (!task_ptr) {
            unsigned attempts = 0;
            unsigned max_attempts = num_threads_ * 2;
            while (!task_ptr && attempts < max_attempts) {
                unsigned victim = dist(rng);
                if (victim != id) {
                    if (auto t = deques_[victim]->steal()) {
                        task_ptr = *t;
                    }
                }
                ++attempts;
            }
        }

        // 4. Execute if we got a task
        if (task_ptr) {
            (*task_ptr)();
            delete task_ptr;
            continue;
        }

        // 5. No work found — check stop flag
        if (stop_.load(std::memory_order_acquire)) {
            // Drain own deque before exiting
            while (auto t = deques_[id]->pop()) {
                (*(*t))();
                delete *t;
            }
            // Drain own submission queue too
            while (auto s = submission_queues_[id]->pop()) {
                (*(*s))();
                delete *s;
            }
            return;
        }

        // 6. Lock-free idle: snapshot the epoch, spin briefly, then
        //    sleep in short increments until the epoch changes.
        //    No mutex is ever taken on this path.
        uint64_t snapshot = epoch_.load(std::memory_order_acquire);

        // Brief spin — gives submitters a window before we sleep.
        for (int spin = 0; spin < 32; ++spin) {
            if (epoch_.load(std::memory_order_acquire) != snapshot) break;
            if (!deques_[id]->empty() || !submission_queues_[id]->empty()) break;
            std::this_thread::yield();
        }

        // If still idle, sleep in short bursts (avoids burning CPU).
        // Each iteration re-checks stop, own queues, and epoch.
        if (epoch_.load(std::memory_order_acquire) == snapshot
            && deques_[id]->empty() && submission_queues_[id]->empty()) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

bool ThreadPool::try_process_one(unsigned id) {
    Task task_ptr = nullptr;

    // 1. Try own deque (subtasks from current workflow).
    if (auto t = deques_[id]->pop()) {
        task_ptr = *t;
    }

    // 2. Try own submission queue.
    if (!task_ptr) {
        if (auto s = submission_queues_[id]->pop()) {
            task_ptr = *s;
        }
    }

    // 3. Try stealing from a random peer's deque.
    if (!task_ptr) {
        for (unsigned i = 0; i < num_threads_; ++i) {
            if (i != id) {
                if (auto t = deques_[i]->steal()) {
                    task_ptr = *t;
                    break;
                }
            }
        }
    }

    if (task_ptr) {
        (*task_ptr)();
        delete task_ptr;
        return true;
    }
    return false;
}

void ThreadPool::notify() {
    // Lock-free: bump epoch so sleeping workers detect new work.
    epoch_.fetch_add(1, std::memory_order_release);
}

}  // namespace forge
