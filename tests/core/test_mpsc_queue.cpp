// Tests for the lock-free MPSC queue.
#include <catch2/catch_test_macros.hpp>

#include "core/mpsc_queue.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

TEST_CASE("MPSCQueue: single producer, single consumer", "[mpsc]") {
    forge::MPSCQueue<int> q;

    SECTION("empty queue returns nullopt") {
        REQUIRE(q.empty());
        REQUIRE(q.pop() == std::nullopt);
    }

    SECTION("push then pop returns value") {
        q.push(42);
        REQUIRE_FALSE(q.empty());
        auto val = q.pop();
        REQUIRE(val.has_value());
        REQUIRE(*val == 42);
        REQUIRE(q.empty());
    }

    SECTION("FIFO ordering preserved") {
        for (int i = 0; i < 100; ++i) {
            q.push(i);
        }
        for (int i = 0; i < 100; ++i) {
            auto val = q.pop();
            REQUIRE(val.has_value());
            REQUIRE(*val == i);
        }
        REQUIRE(q.pop() == std::nullopt);
    }

    SECTION("interleaved push and pop") {
        q.push(1);
        q.push(2);
        REQUIRE(*q.pop() == 1);
        q.push(3);
        REQUIRE(*q.pop() == 2);
        REQUIRE(*q.pop() == 3);
        REQUIRE(q.pop() == std::nullopt);
    }
}

TEST_CASE("MPSCQueue: move-only types", "[mpsc]") {
    forge::MPSCQueue<std::unique_ptr<int>> q;

    q.push(std::make_unique<int>(99));
    auto val = q.pop();
    REQUIRE(val.has_value());
    REQUIRE(**val == 99);
}

TEST_CASE("MPSCQueue: multi-producer stress test", "[mpsc][stress]") {
    forge::MPSCQueue<int> q;

    constexpr int NUM_PRODUCERS = 8;
    constexpr int ITEMS_PER_PRODUCER = 10000;

    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);

    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&q, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                q.push(p * ITEMS_PER_PRODUCER + i);
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // Consumer: drain all items and verify count + uniqueness
    std::set<int> received;
    while (true) {
        auto val = q.pop();
        if (!val) break;
        received.insert(*val);
    }

    REQUIRE(received.size() == NUM_PRODUCERS * ITEMS_PER_PRODUCER);

    // Verify every expected value is present
    for (int i = 0; i < NUM_PRODUCERS * ITEMS_PER_PRODUCER; ++i) {
        REQUIRE(received.count(i) == 1);
    }
}

TEST_CASE("MPSCQueue: concurrent push and pop", "[mpsc][stress]") {
    forge::MPSCQueue<int> q;

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 5000;
    std::atomic<int> total_popped{0};

    // Start consumer thread
    std::atomic<bool> producers_done{false};
    std::thread consumer([&]() {
        while (!producers_done.load(std::memory_order_acquire) || !q.empty()) {
            auto val = q.pop();
            if (val) {
                total_popped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Final drain
        while (auto val = q.pop()) {
            total_popped.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Start producers
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&q, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                q.push(p * ITEMS_PER_PRODUCER + i);
            }
        });
    }

    for (auto& t : producers) t.join();
    producers_done.store(true, std::memory_order_release);
    consumer.join();

    REQUIRE(total_popped.load() == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}
