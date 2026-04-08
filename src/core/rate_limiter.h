#pragma once
// Token bucket rate limiter.
// Thread-safe, lock-free (atomics only).
// Supports non-blocking try_acquire() and blocking acquire().

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>

namespace forge {

class RateLimiter {
public:
    /// Construct a rate limiter.
    /// @param tokens_per_second  Sustained rate
    /// @param burst_size         Max tokens that can be consumed in a burst
    RateLimiter(double tokens_per_second, double burst_size)
        : rate_(tokens_per_second),
          burst_(burst_size) {
        // Store tokens as integer (scaled by 1e6 for sub-token precision)
        tokens_.store(to_fixed(burst_size), std::memory_order_relaxed);
        last_refill_.store(now_us(), std::memory_order_relaxed);
    }

    /// Non-blocking acquire. Returns true if a token was available.
    bool try_acquire(double amount = 1.0) {
        refill();
        int64_t needed = to_fixed(amount);
        int64_t current = tokens_.load(std::memory_order_relaxed);
        while (true) {
            if (current < needed) {
                return false;
            }
            if (tokens_.compare_exchange_weak(current, current - needed,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
            // current is updated by CAS on failure — retry
        }
    }

    /// Blocking acquire — spins (with backoff) until a token is available.
    void acquire(double amount = 1.0) {
        while (!try_acquire(amount)) {
            // Brief sleep to avoid pure busy-wait
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    /// Current (approximate) token count.
    double available() const {
        return from_fixed(tokens_.load(std::memory_order_relaxed));
    }

    /// Rate in tokens per second.
    double rate() const { return rate_; }

    /// Maximum burst size.
    double burst() const { return burst_; }

private:
    static constexpr int64_t SCALE = 1'000'000;  // Fixed-point scale

    static int64_t to_fixed(double v) {
        return static_cast<int64_t>(v * SCALE);
    }

    static double from_fixed(int64_t v) {
        return static_cast<double>(v) / SCALE;
    }

    static int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void refill() {
        int64_t now = now_us();
        int64_t prev = last_refill_.load(std::memory_order_relaxed);
        int64_t elapsed_us = now - prev;
        if (elapsed_us <= 0) return;

        // CAS loop: every thread computes tokens for the window it claims.
        // On CAS failure we reload prev and retry with the new window,
        // so no thread's refill contribution is lost.
        while (true) {
            if (!last_refill_.compare_exchange_weak(prev, now,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Another thread moved the timestamp. Recompute our window.
                elapsed_us = now - prev;
                if (elapsed_us <= 0) return;
                continue;
            }
            break;
        }

        int64_t new_tokens = static_cast<int64_t>(
            (rate_ * static_cast<double>(elapsed_us) / 1'000'000.0) * SCALE);
        if (new_tokens <= 0) return;

        int64_t max_tokens = to_fixed(burst_);
        int64_t current = tokens_.load(std::memory_order_relaxed);
        int64_t desired;
        do {
            desired = std::min(current + new_tokens, max_tokens);
        } while (!tokens_.compare_exchange_weak(current, desired,
                     std::memory_order_acq_rel, std::memory_order_relaxed));
    }

    double rate_;     // tokens per second
    double burst_;    // max bucket capacity

    alignas(64) std::atomic<int64_t> tokens_;        // fixed-point token count
    alignas(64) std::atomic<int64_t> last_refill_;   // last refill timestamp (μs)
};

}  // namespace forge
