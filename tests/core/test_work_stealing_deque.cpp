// Tests for the Chase-Lev work-stealing deque.
#include <catch2/catch_test_macros.hpp>

#include "core/work_stealing_deque.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

TEST_CASE("WorkStealingDeque: basic owner operations", "[deque]") {
    forge::WorkStealingDeque<int> dq;

    SECTION("empty deque returns nullopt on pop and steal") {
        REQUIRE(dq.empty());
        REQUIRE(dq.pop() == std::nullopt);
        REQUIRE(dq.steal() == std::nullopt);
    }

    SECTION("push and pop (LIFO)") {
        dq.push(1);
        dq.push(2);
        dq.push(3);

        REQUIRE(dq.size() == 3);

        // Pop is LIFO — should get 3, 2, 1
        REQUIRE(*dq.pop() == 3);
        REQUIRE(*dq.pop() == 2);
        REQUIRE(*dq.pop() == 1);
        REQUIRE(dq.pop() == std::nullopt);
    }

    SECTION("push and steal (FIFO)") {
        dq.push(1);
        dq.push(2);
        dq.push(3);

        // Steal is FIFO — should get 1, 2, 3
        REQUIRE(*dq.steal() == 1);
        REQUIRE(*dq.steal() == 2);
        REQUIRE(*dq.steal() == 3);
        REQUIRE(dq.steal() == std::nullopt);
    }
}

TEST_CASE("WorkStealingDeque: growth beyond initial capacity", "[deque]") {
    forge::WorkStealingDeque<int> dq(4);  // small initial capacity

    for (int i = 0; i < 100; ++i) {
        dq.push(i);
    }

    REQUIRE(dq.size() == 100);

    // Pop all — LIFO so 99, 98, ..., 0
    for (int i = 99; i >= 0; --i) {
        auto val = dq.pop();
        REQUIRE(val.has_value());
        REQUIRE(*val == i);
    }
}

TEST_CASE("WorkStealingDeque: concurrent steal stress", "[deque][stress]") {
    forge::WorkStealingDeque<int> dq;

    constexpr int NUM_ITEMS = 10000;
    constexpr int NUM_THIEVES = 4;

    // Owner pushes all items first
    for (int i = 0; i < NUM_ITEMS; ++i) {
        dq.push(i);
    }

    // Thieves steal concurrently
    std::vector<std::vector<int>> stolen(NUM_THIEVES);
    std::vector<std::thread> thieves;
    thieves.reserve(NUM_THIEVES);

    for (int t = 0; t < NUM_THIEVES; ++t) {
        thieves.emplace_back([&dq, &stolen, t]() {
            while (true) {
                auto val = dq.steal();
                if (!val) break;
                stolen[t].push_back(*val);
            }
        });
    }

    for (auto& t : thieves) t.join();

    // Collect all stolen items + any remaining in deque
    std::set<int> all;
    for (auto& vec : stolen) {
        for (int v : vec) {
            all.insert(v);
        }
    }
    while (auto val = dq.pop()) {
        all.insert(*val);
    }

    // Every item should appear exactly once
    REQUIRE(all.size() == NUM_ITEMS);
    for (int i = 0; i < NUM_ITEMS; ++i) {
        REQUIRE(all.count(i) == 1);
    }
}

TEST_CASE("WorkStealingDeque: owner push + pop with concurrent stealers", "[deque][stress]") {
    forge::WorkStealingDeque<int> dq;

    constexpr int NUM_ITEMS = 20000;
    constexpr int NUM_THIEVES = 3;

    std::atomic<bool> done{false};
    std::vector<std::vector<int>> stolen(NUM_THIEVES);
    std::vector<int> owner_got;

    // Thieves
    std::vector<std::thread> thieves;
    for (int t = 0; t < NUM_THIEVES; ++t) {
        thieves.emplace_back([&dq, &stolen, &done, t]() {
            while (!done.load(std::memory_order_acquire)) {
                auto val = dq.steal();
                if (val) {
                    stolen[t].push_back(*val);
                }
            }
            // Final drain
            while (auto val = dq.steal()) {
                stolen[t].push_back(*val);
            }
        });
    }

    // Owner: push and pop interleaved
    for (int i = 0; i < NUM_ITEMS; ++i) {
        dq.push(i);
        if (i % 3 == 0) {
            auto val = dq.pop();
            if (val) owner_got.push_back(*val);
        }
    }
    // Drain remaining from owner side
    while (auto val = dq.pop()) {
        owner_got.push_back(*val);
    }

    done.store(true, std::memory_order_release);
    for (auto& t : thieves) t.join();

    // Verify: every pushed item appears exactly once across owner + all thieves
    std::set<int> all;
    for (int v : owner_got) all.insert(v);
    for (auto& vec : stolen) {
        for (int v : vec) all.insert(v);
    }

    REQUIRE(all.size() == NUM_ITEMS);
}
