#pragma once
// Lock-free counting semaphore.
// Used for capping concurrent in-flight LLM requests across all sessions.

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

namespace forge {

class Semaphore {
public:
    explicit Semaphore(int max_permits)
        : permits_(max_permits), max_permits_(max_permits) {
        assert(max_permits > 0);
    }

    /// Try to acquire one permit.  Returns false immediately if none available.
    bool try_acquire() {
        int current = permits_.load(std::memory_order_relaxed);
        while (current > 0) {
            if (permits_.compare_exchange_weak(current, current - 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            }
            // current is updated by CAS on failure — retry.
        }
        return false;
    }

    /// Acquire one permit, blocking with exponential backoff until available.
    void acquire() {
        unsigned iter = 0;
        while (!try_acquire()) {
            if (iter < 4) {
                // Busy-spin.
            } else if (iter < 16) {
                std::this_thread::yield();
            } else if (iter < 64) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            ++iter;
        }
    }

    /// Release one permit.
    void release() {
        int prev = permits_.fetch_add(1, std::memory_order_release);
        // Guard against over-release.
        assert(prev < max_permits_ && "Semaphore released more than acquired");
        (void)prev;
    }

    /// Current number of available permits (approximate under contention).
    int available() const {
        return permits_.load(std::memory_order_relaxed);
    }

    /// Maximum permits.
    int max_permits() const { return max_permits_; }

private:
    alignas(64) std::atomic<int> permits_;
    int max_permits_;
};

}  // namespace forge
