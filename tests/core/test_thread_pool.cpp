// Tests for the work-stealing thread pool.
#include <catch2/catch_test_macros.hpp>

#include "core/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

TEST_CASE("ThreadPool: submit and get result", "[pool]") {
    forge::ThreadPool pool(4);

    auto fut = pool.submit([]() -> int { return 42; });
    REQUIRE(fut.get() == 42);
}

TEST_CASE("ThreadPool: submit many tasks", "[pool]") {
    forge::ThreadPool pool(4);
    constexpr int N = 1000;

    std::vector<forge::Future<int>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.submit([i]() -> int { return i * i; }));
    }

    for (int i = 0; i < N; ++i) {
        REQUIRE(futures[i].get() == i * i);
    }
}

TEST_CASE("ThreadPool: tasks execute concurrently", "[pool]") {
    forge::ThreadPool pool(4);

    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    constexpr int N = 20;
    std::vector<forge::Future<int>> futures;

    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.submit([&]() -> int {
            int c = concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
            // Track maximum observed concurrency
            int prev_max = max_concurrent.load(std::memory_order_relaxed);
            while (prev_max < c &&
                   !max_concurrent.compare_exchange_weak(prev_max, c,
                       std::memory_order_relaxed)) {}

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            concurrent.fetch_sub(1, std::memory_order_relaxed);
            return c;
        }));
    }

    for (auto& f : futures) f.get();

    // With 4 threads and 20 tasks that sleep, we should see > 1 concurrent
    REQUIRE(max_concurrent.load() > 1);
}

TEST_CASE("ThreadPool: exception propagation", "[pool]") {
    forge::ThreadPool pool(2);

    auto fut = pool.submit([]() -> int {
        throw std::runtime_error("test error");
    });

    REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("ThreadPool: shutdown with pending tasks", "[pool]") {
    // All submitted tasks should complete even during shutdown
    std::atomic<int> completed{0};
    constexpr int N = 100;

    {
        forge::ThreadPool pool(2);
        for (int i = 0; i < N; ++i) {
            pool.submit([&completed]() -> int {
                completed.fetch_add(1, std::memory_order_relaxed);
                return 0;
            });
        }
        // Pool destructor runs here — should drain remaining tasks
    }

    REQUIRE(completed.load() == N);
}

TEST_CASE("ThreadPool: Future::then continuation", "[pool]") {
    forge::ThreadPool pool(2);

    auto fut = pool.submit([]() -> int { return 10; });

    std::atomic<int> callback_result{0};
    fut.then([&callback_result](int val) {
        callback_result.store(val * 2, std::memory_order_relaxed);
    });

    // Give time for continuation to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(callback_result.load() == 20);
}

TEST_CASE("ThreadPool: Future::wait_for timeout", "[pool]") {
    forge::ThreadPool pool(1);

    auto fut = pool.submit([]() -> int {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 99;
    });

    auto result = fut.wait_for(std::chrono::milliseconds(10));
    REQUIRE_FALSE(result.has_value());  // should timeout
}

TEST_CASE("ThreadPool: default thread count uses hardware_concurrency", "[pool]") {
    forge::ThreadPool pool;
    unsigned expected = std::max(1u, std::thread::hardware_concurrency());
    REQUIRE(pool.num_threads() == expected);
}

TEST_CASE("ThreadPool: void tasks", "[pool]") {
    forge::ThreadPool pool(2);
    auto fut = pool.submit([]() { /* void return */ });
    fut.get();
    REQUIRE(true); // If it compiles and runs without exception, we pass
}

TEST_CASE("ThreadPool: submit_local nested tasks", "[pool]") {
    forge::ThreadPool pool(2);

    std::atomic<int> subtask_executed{0};

    auto root_fut = pool.submit([&pool, &subtask_executed]() -> int {
        auto sub1 = pool.submit_local([&subtask_executed]() {
            subtask_executed.fetch_add(1, std::memory_order_relaxed);
        });
        auto sub2 = pool.submit_local([&subtask_executed]() {
            subtask_executed.fetch_add(1, std::memory_order_relaxed);
        });

        sub1.get();
        sub2.get();

        return 42;
    });

    REQUIRE(root_fut.get() == 42);
    REQUIRE(subtask_executed.load() == 2);
}
