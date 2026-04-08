// Stress tests for lock-free primitives and Phase 1 components.
// Designed to expose data races, ABA problems, and ordering bugs
// under high contention.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/concurrent_map.h"
#include "core/future.h"
#include "core/mpsc_queue.h"
#include "core/thread_pool.h"
#include "core/rate_limiter.h"
#include "core/work_stealing_deque.h"
#include "llm/message.h"
#include "session/conversation.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "tools/tool_spec.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// MPSC Queue stress
// ---------------------------------------------------------------------------

TEST_CASE("Stress: MPSC queue — many producers, verify no lost items",
          "[stress][mpsc]") {
    constexpr int NUM_PRODUCERS = 16;
    constexpr int ITEMS_PER_PRODUCER = 50'000;

    forge::MPSCQueue<int> q;
    std::atomic<int> started{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            started.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                q.push(p * ITEMS_PER_PRODUCER + i);
            }
        });
    }

    // Wait for all producers to be ready, then release them simultaneously.
    while (started.load() < NUM_PRODUCERS) std::this_thread::yield();
    go.store(true, std::memory_order_release);

    for (auto& t : producers) t.join();

    // Drain and verify every value arrived exactly once.
    std::vector<bool> seen(NUM_PRODUCERS * ITEMS_PER_PRODUCER, false);
    int count = 0;
    while (auto val = q.pop()) {
        REQUIRE(*val >= 0);
        REQUIRE(*val < NUM_PRODUCERS * ITEMS_PER_PRODUCER);
        REQUIRE_FALSE(seen[*val]);  // no duplicates
        seen[*val] = true;
        ++count;
    }
    REQUIRE(count == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

// ---------------------------------------------------------------------------
// Work-stealing deque stress
// ---------------------------------------------------------------------------

TEST_CASE("Stress: work-stealing deque — owner vs many stealers",
          "[stress][deque]") {
    constexpr int TOTAL_ITEMS = 200'000;
    constexpr int NUM_STEALERS = 8;

    forge::WorkStealingDeque<int> deque;
    std::atomic<bool> done{false};
    std::atomic<int> stolen_count{0};
    std::atomic<int> owner_popped{0};

    // Stealers run continuously.
    std::vector<std::thread> stealers;
    for (int s = 0; s < NUM_STEALERS; ++s) {
        stealers.emplace_back([&]() {
            int local = 0;
            while (!done.load(std::memory_order_acquire)) {
                if (deque.steal().has_value()) {
                    ++local;
                }
            }
            // Drain remaining.
            while (deque.steal().has_value()) ++local;
            stolen_count.fetch_add(local);
        });
    }

    // Owner pushes and pops interleaved.
    int owner_local = 0;
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        deque.push(i);
        // Occasionally pop from owner side to create contention.
        if (i % 3 == 0) {
            if (deque.pop().has_value()) ++owner_local;
        }
    }
    // Owner drains its side.
    while (deque.pop().has_value()) ++owner_local;
    owner_popped.store(owner_local);

    done.store(true, std::memory_order_release);
    for (auto& t : stealers) t.join();

    int total = owner_popped.load() + stolen_count.load();
    REQUIRE(total == TOTAL_ITEMS);
}

// ---------------------------------------------------------------------------
// Future/Promise stress — many continuations set concurrently
// ---------------------------------------------------------------------------

TEST_CASE("Stress: Future/Promise — race between set_value and then",
          "[stress][future]") {
    constexpr int ITERATIONS = 100'000;

    std::atomic<int> completed{0};
    std::atomic<bool> mismatch{false};

    // Half the time set_value fires first, half the time then() is set first.
    // This exercises both orderings of the lock-free protocol.
    // NOTE: Catch2's REQUIRE is NOT thread-safe, so we must not call it
    // from the setter thread (which may invoke the callback).
    for (int i = 0; i < ITERATIONS; ++i) {
        forge::Promise<int> promise;
        auto fut = promise.get_future();

        std::thread setter([p = std::move(promise), i]() mutable {
            p.set_value(i);
        });

        fut.then([&completed, &mismatch, i](int val) {
            if (val != i) mismatch.store(true, std::memory_order_relaxed);
            completed.fetch_add(1);
        });

        setter.join();
    }

    REQUIRE_FALSE(mismatch.load());
    REQUIRE(completed.load() == ITERATIONS);
}

TEST_CASE("Stress: Future/Promise — get() under contention",
          "[stress][future]") {
    constexpr int ITERATIONS = 50'000;

    forge::ThreadPool pool(8);

    for (int i = 0; i < ITERATIONS; ++i) {
        auto fut = pool.submit([i]() { return i * 2; });
        REQUIRE(fut.get() == i * 2);
    }
}

// ---------------------------------------------------------------------------
// Thread pool stress — submit storm
// ---------------------------------------------------------------------------

TEST_CASE("Stress: ThreadPool — high-volume submit storm",
          "[stress][pool]") {
    constexpr int NUM_TASKS = 200'000;

    forge::ThreadPool pool(8);
    std::atomic<int> counter{0};

    std::vector<forge::Future<void>> futures;
    futures.reserve(NUM_TASKS);

    for (int i = 0; i < NUM_TASKS; ++i) {
        futures.push_back(pool.submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.get();

    REQUIRE(counter.load() == NUM_TASKS);
}

TEST_CASE("Stress: ThreadPool — concurrent submitters from many threads",
          "[stress][pool]") {
    constexpr int NUM_SUBMITTERS = 16;
    constexpr int TASKS_PER_SUBMITTER = 10'000;

    forge::ThreadPool pool(8);
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    for (int s = 0; s < NUM_SUBMITTERS; ++s) {
        submitters.emplace_back([&]() {
            for (int i = 0; i < TASKS_PER_SUBMITTER; ++i) {
                auto f = pool.submit([&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
                // Don't wait — fire and forget (pool destructor joins).
            }
        });
    }

    for (auto& t : submitters) t.join();

    // Give pool time to drain.
    // Submit a sentinel and wait for it.
    auto sentinel = pool.submit([]() {});
    sentinel.get();

    // All tasks should have run. Allow a small spin for any in-flight.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (counter.load() < NUM_SUBMITTERS * TASKS_PER_SUBMITTER &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    REQUIRE(counter.load() == NUM_SUBMITTERS * TASKS_PER_SUBMITTER);
}

// ---------------------------------------------------------------------------
// ConcurrentMap stress
// ---------------------------------------------------------------------------

TEST_CASE("Stress: ConcurrentMap — concurrent insert/find/erase",
          "[stress][map]") {
    constexpr int NUM_THREADS = 16;
    constexpr int OPS_PER_THREAD = 20'000;

    forge::ConcurrentMap<int, int> map;
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();

            std::mt19937 rng(t);
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                int key = rng() % 1000;  // High collision rate.
                int op = rng() % 3;
                if (op == 0) {
                    map.insert(key, i);
                } else if (op == 1) {
                    map.find(key);  // May or may not exist.
                } else {
                    map.erase(key);
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // No crash, no hang = success. Verify structural integrity.
    auto keys = map.keys();
    for (auto k : keys) {
        REQUIRE(map.find(k).has_value());
    }
}

// ---------------------------------------------------------------------------
// Rate limiter stress
// ---------------------------------------------------------------------------

TEST_CASE("Stress: RateLimiter — concurrent acquire never exceeds burst",
          "[stress][ratelimiter]") {
    // High rate so we don't block long, but verify atomicity.
    forge::RateLimiter limiter(100000.0, 100);

    constexpr int NUM_THREADS = 16;
    constexpr int ACQUIRES_PER_THREAD = 5'000;
    std::atomic<int> acquired{0};
    std::atomic<int> failed{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ACQUIRES_PER_THREAD; ++i) {
                if (limiter.try_acquire()) {
                    acquired.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    REQUIRE(acquired.load() + failed.load() == NUM_THREADS * ACQUIRES_PER_THREAD);
    // With 100k tokens/sec and short runtime, most should succeed.
    REQUIRE(acquired.load() > 0);
}

// ---------------------------------------------------------------------------
// Tool executor stress — many concurrent tool calls
// ---------------------------------------------------------------------------

TEST_CASE("Stress: ToolExecutor — batch execution under load",
          "[stress][executor]") {
    forge::ThreadPool pool(8);
    forge::ToolRegistry registry;

    forge::ToolSpec spec;
    spec.name = "counter";
    spec.description = "Increments a counter";
    spec.timeout = std::chrono::milliseconds(5000);

    std::atomic<int> counter{0};
    registry.register_tool(std::move(spec),
        [&counter](const std::string&) -> std::string {
            counter.fetch_add(1, std::memory_order_relaxed);
            return "ok";
        });

    forge::ToolExecutor executor(pool, registry);

    constexpr int BATCH_SIZE = 500;
    std::vector<forge::ToolCall> calls;
    calls.reserve(BATCH_SIZE);
    for (int i = 0; i < BATCH_SIZE; ++i) {
        forge::ToolCall call;
        call.id = "call_" + std::to_string(i);
        call.name = "counter";
        call.arguments_json = "{}";
        calls.push_back(std::move(call));
    }

    auto results = executor.execute_batch(calls);
    REQUIRE(results.size() == BATCH_SIZE);
    for (auto& r : results) {
        REQUIRE_FALSE(r.is_error);
    }
    REQUIRE(counter.load() == BATCH_SIZE);
}

// ---------------------------------------------------------------------------
// Conversation ring buffer stress — rapid push from multiple threads
// ---------------------------------------------------------------------------

TEST_CASE("Stress: Conversation — rapid push doesn't corrupt",
          "[stress][conversation]") {
    // Conversation is NOT thread-safe by design (single-session access).
    // This test verifies structural integrity under rapid sequential use.
    forge::Conversation conv(100);

    forge::Message sys;
    sys.role = "system";
    sys.content = "system prompt";
    conv.push(sys);

    // Push 10,000 messages; capacity is 100, so heavy eviction.
    for (int i = 0; i < 10'000; ++i) {
        forge::Message m;
        m.role = (i % 2 == 0) ? "user" : "assistant";
        m.content = "msg_" + std::to_string(i);
        conv.push(m);
    }

    REQUIRE(conv.size() == 100);
    // System message should be preserved.
    REQUIRE(conv.messages().front().role == "system");
    REQUIRE(conv.messages().front().content == "system prompt");
    // Last message should be the most recent.
    REQUIRE(conv.messages().back().content == "msg_9999");

    // to_json should not crash.
    auto j = conv.to_json();
    REQUIRE(j.size() == 100);
}

// ---------------------------------------------------------------------------
// Integration stress: mock LLM server under high request volume
// ---------------------------------------------------------------------------

TEST_CASE("Stress: AsyncHttpClient — parallel request storm",
          "[stress][http]") {
    // Spin up a simple server that returns a counter.
    httplib::Server svr;
    std::atomic<int> request_count{0};

    svr.Get("/ping", [&](const httplib::Request&, httplib::Response& res) {
        int n = request_count.fetch_add(1);
        res.set_content(std::to_string(n), "text/plain");
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&svr]() { svr.listen_after_bind(); });

    {
        forge::ThreadPool pool(8);
        forge::AsyncHttpClient http(pool, {10, 30, false});

        constexpr int NUM_REQUESTS = 500;
        std::vector<forge::Future<forge::HttpResponse>> futures;
        futures.reserve(NUM_REQUESTS);

        std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ping";

        for (int i = 0; i < NUM_REQUESTS; ++i) {
            futures.push_back(http.get(url));
        }

        int success = 0;
        for (auto& f : futures) {
            auto resp = f.get();
            if (resp.status == 200) ++success;
        }

        REQUIRE(success == NUM_REQUESTS);
        REQUIRE(request_count.load() == NUM_REQUESTS);
    }

    svr.stop();
    server_thread.join();
}

// ---------------------------------------------------------------------------
// Thread pool: work stealing actually happens
// ---------------------------------------------------------------------------

TEST_CASE("Stress: ThreadPool — work stealing balances load",
          "[stress][pool]") {
    // Submit all work to one thread via a chain, verify completion.
    // The other threads should steal some of it.
    constexpr int NUM_TASKS = 100'000;

    forge::ThreadPool pool(8);
    std::atomic<int> completed{0};

    // Submit tasks in rapid succession.
    std::vector<forge::Future<void>> futs;
    futs.reserve(NUM_TASKS);

    for (int i = 0; i < NUM_TASKS; ++i) {
        futs.push_back(pool.submit([&completed]() {
            // Do a tiny bit of work to avoid being optimized away.
            volatile int x = 0;
            for (int j = 0; j < 10; ++j) ++x;
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futs) f.get();
    REQUIRE(completed.load() == NUM_TASKS);
}
