#include "core/thread_pool.h"

#include <spdlog/spdlog.h>

namespace forge {

thread_local int t_worker_id = -1;

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
    // Wake all sleeping workers so they observe the stop flag
    wake_cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    // All workers have exited. Single-threaded drain of any remaining
    // tasks in the submission queues (shouldn't happen normally, but be safe).
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

        // 2. Try submission queue (FIFO - optimized for root tasks)
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

        // 6. Sleep briefly to avoid busy-spinning
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            sleeping_count_.fetch_add(1, std::memory_order_seq_cst);
            // Re-check queues under lock to avoid sleep/wake race condition
            if (deques_[id]->empty() && submission_queues_[id]->empty()) {
                wake_cv_.wait_for(lock, std::chrono::microseconds(200), [this, id] {
                    return stop_.load(std::memory_order_relaxed) || !submission_queues_[id]->empty();
                });
            }
            sleeping_count_.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
}

void ThreadPool::wake_one() {
    if (sleeping_count_.load(std::memory_order_seq_cst) > 0) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_cv_.notify_one();
    }
}

}  // namespace forge
