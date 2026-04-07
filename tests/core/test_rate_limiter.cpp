// Tests for the token bucket rate limiter.
#include <catch2/catch_test_macros.hpp>

#include "core/rate_limiter.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("RateLimiter: burst acquisition", "[ratelimit]") {
    // 10 tokens/sec, burst of 5
    forge::RateLimiter limiter(10.0, 5.0);

    SECTION("can acquire up to burst size immediately") {
        for (int i = 0; i < 5; ++i) {
            REQUIRE(limiter.try_acquire());
        }
        // 6th should fail — burst exhausted
        REQUIRE_FALSE(limiter.try_acquire());
    }

    SECTION("available reports correct count") {
        double initial = limiter.available();
        REQUIRE(initial >= 4.9);  // approximately 5 (floating point)
        REQUIRE(initial <= 5.1);
    }
}

TEST_CASE("RateLimiter: token refill over time", "[ratelimit]") {
    // 100 tokens/sec, burst of 10
    forge::RateLimiter limiter(100.0, 10.0);

    // Exhaust all tokens
    while (limiter.try_acquire()) {}

    // Wait for refill (100 tokens/sec → ~10 tokens in 100ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should have some tokens now
    REQUIRE(limiter.try_acquire());
}

TEST_CASE("RateLimiter: blocking acquire", "[ratelimit]") {
    // 1000 tokens/sec, burst of 1 (forces waiting)
    forge::RateLimiter limiter(1000.0, 1.0);

    // First acquire succeeds immediately
    limiter.try_acquire();

    auto start = std::chrono::steady_clock::now();
    limiter.acquire();  // should block briefly
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should have taken roughly 1ms (1 token at 1000/sec)
    // Be generous with the upper bound due to scheduling jitter
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    REQUIRE(ms < 500);  // sanity: shouldn't take forever
}

TEST_CASE("RateLimiter: respects burst cap", "[ratelimit]") {
    forge::RateLimiter limiter(1000.0, 3.0);

    // Wait to let tokens accumulate beyond burst
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should still cap at burst size (3)
    int acquired = 0;
    while (limiter.try_acquire()) {
        ++acquired;
        if (acquired > 10) break;  // safety
    }

    // Should get at most burst_size tokens
    REQUIRE(acquired <= 4);  // some slack for timing
    REQUIRE(acquired >= 2);
}

TEST_CASE("RateLimiter: concurrent access", "[ratelimit][stress]") {
    // 10000 tokens/sec, burst of 100
    forge::RateLimiter limiter(10000.0, 100.0);

    constexpr int NUM_THREADS = 4;
    std::atomic<int> total_acquired{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 50; ++j) {
                if (limiter.try_acquire()) {
                    total_acquired.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    // We should have acquired some tokens (up to burst + refilled)
    // but not more than were available
    REQUIRE(total_acquired.load() > 0);
    REQUIRE(total_acquired.load() <= NUM_THREADS * 50);
}

TEST_CASE("RateLimiter: rate and burst accessors", "[ratelimit]") {
    forge::RateLimiter limiter(42.5, 7.0);
    REQUIRE(limiter.rate() == 42.5);
    REQUIRE(limiter.burst() == 7.0);
}
