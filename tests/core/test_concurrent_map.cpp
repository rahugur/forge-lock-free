// Tests for the striped concurrent hash map.
#include <catch2/catch_test_macros.hpp>

#include "core/concurrent_map.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("ConcurrentMap: basic insert/find/erase", "[concmap]") {
    forge::ConcurrentMap<int, std::string> m;

    SECTION("empty map") {
        REQUIRE(m.empty());
        REQUIRE(m.size() == 0);
        REQUIRE(m.find(1) == std::nullopt);
        REQUIRE_FALSE(m.contains(1));
    }

    SECTION("insert and find") {
        REQUIRE(m.insert(1, "one"));
        REQUIRE(m.insert(2, "two"));
        REQUIRE(m.insert(3, "three"));

        REQUIRE(m.size() == 3);
        REQUIRE(m.find(1) == "one");
        REQUIRE(m.find(2) == "two");
        REQUIRE(m.find(3) == "three");
        REQUIRE(m.find(4) == std::nullopt);
    }

    SECTION("insert overwrites existing key") {
        m.insert(1, "one");
        REQUIRE_FALSE(m.insert(1, "ONE"));  // returns false (not newly inserted)
        REQUIRE(m.find(1) == "ONE");        // value was updated
    }

    SECTION("erase") {
        m.insert(1, "one");
        m.insert(2, "two");

        REQUIRE(m.erase(1));
        REQUIRE_FALSE(m.erase(1));  // already erased
        REQUIRE(m.find(1) == std::nullopt);
        REQUIRE(m.find(2) == "two");
        REQUIRE(m.size() == 1);
    }

    SECTION("contains") {
        m.insert(1, "one");
        REQUIRE(m.contains(1));
        REQUIRE_FALSE(m.contains(2));
    }

    SECTION("keys()") {
        m.insert(10, "ten");
        m.insert(20, "twenty");
        m.insert(30, "thirty");

        auto keys = m.keys();
        std::set<int> key_set(keys.begin(), keys.end());
        REQUIRE(key_set == std::set<int>{10, 20, 30});
    }
}

TEST_CASE("ConcurrentMap: with_value and mutate visitors", "[concmap]") {
    forge::ConcurrentMap<int, int> m;
    m.insert(1, 100);

    SECTION("with_value reads under shared lock") {
        int observed = 0;
        REQUIRE(m.with_value(1, [&](const int& v) { observed = v; }));
        REQUIRE(observed == 100);

        REQUIRE_FALSE(m.with_value(99, [](const int&) {}));
    }

    SECTION("mutate modifies under exclusive lock") {
        REQUIRE(m.mutate(1, [](int& v) { v += 50; }));
        REQUIRE(m.find(1) == 150);

        REQUIRE_FALSE(m.mutate(99, [](int&) {}));
    }
}

TEST_CASE("ConcurrentMap: concurrent read/write stress", "[concmap][stress]") {
    forge::ConcurrentMap<int, int> m;

    constexpr int NUM_WRITERS = 2;
    constexpr int NUM_READERS = 8;
    constexpr int ITEMS_PER_WRITER = 5000;

    std::atomic<bool> writers_done{false};

    // Writers: insert and erase
    std::vector<std::thread> writers;
    for (int w = 0; w < NUM_WRITERS; ++w) {
        writers.emplace_back([&m, w]() {
            int base = w * ITEMS_PER_WRITER;
            for (int i = 0; i < ITEMS_PER_WRITER; ++i) {
                m.insert(base + i, i);
            }
            // Erase half
            for (int i = 0; i < ITEMS_PER_WRITER / 2; ++i) {
                m.erase(base + i);
            }
        });
    }

    // Readers: concurrent reads
    std::atomic<int> read_count{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < NUM_READERS; ++r) {
        readers.emplace_back([&m, &writers_done, &read_count]() {
            while (!writers_done.load(std::memory_order_acquire)) {
                for (int i = 0; i < 100; ++i) {
                    m.find(i);
                    m.contains(i);
                    read_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& w : writers) w.join();
    writers_done.store(true, std::memory_order_release);
    for (auto& r : readers) r.join();

    // Verify: each writer's upper half should still exist
    for (int w = 0; w < NUM_WRITERS; ++w) {
        int base = w * ITEMS_PER_WRITER;
        for (int i = ITEMS_PER_WRITER / 2; i < ITEMS_PER_WRITER; ++i) {
            REQUIRE(m.contains(base + i));
        }
    }

    INFO("Total reads performed: " << read_count.load());
}
