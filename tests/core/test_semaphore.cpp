// Tests for the counting semaphore.
#include <catch2/catch_test_macros.hpp>

#include "core/semaphore.h"

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("Semaphore: basic acquire/release", "[semaphore]") {
    forge::Semaphore sem(3);
    REQUIRE(sem.available() == 3);

    REQUIRE(sem.try_acquire());
    REQUIRE(sem.available() == 2);

    REQUIRE(sem.try_acquire());
    REQUIRE(sem.try_acquire());
    REQUIRE(sem.available() == 0);

    // No permits left.
    REQUIRE_FALSE(sem.try_acquire());

    sem.release();
    REQUIRE(sem.available() == 1);
    REQUIRE(sem.try_acquire());
}

TEST_CASE("Semaphore: blocking acquire", "[semaphore]") {
    forge::Semaphore sem(1);
    sem.acquire();  // takes the one permit
    REQUIRE(sem.available() == 0);

    std::atomic<bool> acquired{false};

    std::thread t([&]() {
        sem.acquire();  // will block until release
        acquired.store(true, std::memory_order_release);
        sem.release();
    });

    // Give the thread time to start blocking.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(acquired.load());

    // Release — the blocked thread should proceed.
    sem.release();
    t.join();
    REQUIRE(acquired.load());
}

TEST_CASE("Semaphore: concurrent stress — no over-acquisition", "[semaphore]") {
    constexpr int MAX_PERMITS = 5;
    constexpr int NUM_THREADS = 16;
    constexpr int OPS_PER_THREAD = 5000;

    forge::Semaphore sem(MAX_PERMITS);
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<bool> violation{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                sem.acquire();
                int cur = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
                if (cur > MAX_PERMITS) {
                    violation.store(true, std::memory_order_relaxed);
                }
                // Track max for info.
                int prev_max = max_in_flight.load(std::memory_order_relaxed);
                while (cur > prev_max &&
                       !max_in_flight.compare_exchange_weak(prev_max, cur)) {}

                // Small "work" simulation.
                std::this_thread::yield();

                in_flight.fetch_sub(1, std::memory_order_relaxed);
                sem.release();
            }
        });
    }

    for (auto& t : threads) t.join();

    REQUIRE_FALSE(violation.load());
    REQUIRE(max_in_flight.load() <= MAX_PERMITS);
    REQUIRE(sem.available() == MAX_PERMITS);
}
