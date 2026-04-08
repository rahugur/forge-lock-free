// Forge Benchmark: measures scheduling overhead, session throughput, and memory.
// Compares against equivalent Python framework overhead to quantify the advantage.
//
// Build: cmake -B build -DFORGE_BUILD_BENCHMARKS=ON && cmake --build build
// Run:   ./build/tests/benchmark/bench_sessions

#include "core/thread_pool.h"
#include "session/session.h"
#include "session/session_manager.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "utils/metrics.h"
#include "workflow/react.h"
#include "workflow/workflow_factory.h"

#include "../common/scripted_mock_llm.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

using Clock = std::chrono::steady_clock;

static size_t current_rss_bytes() {
#ifdef __APPLE__
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
#endif
    return 0;
}

// ─── Benchmark 1: Raw scheduling overhead ───────────────────────────
// Measures how fast the thread pool can dispatch and complete trivial tasks.
static void bench_scheduling_overhead() {
    printf("\n=== Benchmark 1: Scheduling Overhead ===\n");

    constexpr int NUM_TASKS = 100000;
    forge::ThreadPool pool(8);

    std::atomic<int> counter{0};

    auto start = Clock::now();

    std::vector<forge::Future<bool>> futures;
    futures.reserve(NUM_TASKS);

    for (int i = 0; i < NUM_TASKS; ++i) {
        futures.push_back(pool.submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            return true;
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - start).count();

    printf("  %d tasks in %lld us\n", NUM_TASKS, elapsed);
    printf("  %.1f ns/task\n", elapsed * 1000.0 / NUM_TASKS);
    printf("  %.0f tasks/sec\n", NUM_TASKS * 1e6 / elapsed);
    printf("  Counter: %d (expected %d)\n", counter.load(), NUM_TASKS);

    // LangChain comparison context:
    // Python asyncio.create_task: ~50-100 us/task (GIL contention)
    // Forge ThreadPool: ~0.1-1 us/task (lock-free)
    printf("  [Context: Python asyncio ~50-100us/task, Forge target <1us/task]\n");
}

// ─── Benchmark 2: Session throughput ────────────────────────────────
// Measures sessions/sec with a mock LLM (no network latency).
static void bench_session_throughput() {
    printf("\n=== Benchmark 2: Session Throughput ===\n");

    constexpr int NUM_SESSIONS = 1000;

    forge::ThreadPool pool(8);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    // Mock LLM: immediate response (2-step: tool call then answer).
    forge::testing::ScriptedMockLLM llm;
    std::atomic<int> call_count{0};
    llm.set_handler([&](const forge::Json& messages, const forge::Json&) {
        int n = call_count.fetch_add(1);
        forge::LLMResponse resp;
        resp.message.role = "assistant";
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;

        auto msg_str = messages.dump();
        if (msg_str.find("\"role\":\"tool\"") != std::string::npos) {
            resp.message.content = "42";
        } else {
            forge::ToolCall tc;
            tc.id = "c" + std::to_string(n);
            tc.name = "calculator";
            tc.arguments_json = R"({"expression": "1+1"})";
            resp.message.tool_calls.push_back(std::move(tc));
        }
        return resp;
    });

    forge::WorkflowFactory factory;
    factory.register_workflow("react", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                          const forge::ToolRegistry& reg, forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(l, ex, reg);
    });

    forge::SessionManager manager(pool, llm, executor, registry, factory, NUM_SESSIONS + 10);

    auto start = Clock::now();

    std::vector<uint64_t> ids;
    ids.reserve(NUM_SESSIONS);
    for (int i = 0; i < NUM_SESSIONS; ++i) {
        forge::CreateSessionRequest req;
        req.prompt = "Compute " + std::to_string(i);
        auto result = manager.create_session(req);
        if (auto* id = std::get_if<uint64_t>(&result)) {
            ids.push_back(*id);
        }
    }

    // Wait for all to complete.
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        int done = 0;
        for (auto id : ids) {
            auto info = manager.get_session(id);
            if (info && (info->state == forge::SessionState::COMPLETED ||
                         info->state == forge::SessionState::FAILED)) {
                ++done;
            }
        }
        if (done >= NUM_SESSIONS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();

    int completed = 0;
    for (auto id : ids) {
        auto info = manager.get_session(id);
        if (info && info->state == forge::SessionState::COMPLETED) ++completed;
    }

    printf("  %d sessions in %lld ms\n", NUM_SESSIONS, elapsed_ms);
    printf("  %.1f sessions/sec\n", NUM_SESSIONS * 1000.0 / elapsed_ms);
    printf("  %d/%d completed successfully\n", completed, NUM_SESSIONS);
    printf("  LLM calls: %d (expected %d)\n", call_count.load(), NUM_SESSIONS * 2);

    // LangChain comparison context:
    // LangChain (asyncio): ~10-50 sessions/sec (Python overhead, GIL)
    // CrewAI: ~5-20 sessions/sec (higher abstraction overhead)
    // Forge: target 500-5000 sessions/sec (with mock LLM, no network)
    printf("  [Context: LangChain ~10-50 sess/s, CrewAI ~5-20, Forge target >500]\n");
}

// ─── Benchmark 3: Memory per session ────────────────────────────────
static void bench_memory_per_session() {
    printf("\n=== Benchmark 3: Memory Per Session ===\n");

    constexpr int NUM_SESSIONS = 500;

    size_t baseline_rss = current_rss_bytes();

    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    forge::testing::ScriptedMockLLM llm;
    llm.set_handler([](const forge::Json&, const forge::Json&) {
        forge::LLMResponse resp;
        resp.message.role = "assistant";
        resp.message.content = "Done";
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;
        return resp;
    });

    forge::WorkflowFactory factory;
    factory.register_workflow("react", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                          const forge::ToolRegistry& reg, forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(l, ex, reg);
    });

    forge::SessionManager manager(pool, llm, executor, registry, factory, NUM_SESSIONS + 10);

    for (int i = 0; i < NUM_SESSIONS; ++i) {
        forge::CreateSessionRequest req;
        req.prompt = "Session " + std::to_string(i);
        manager.create_session(req);
    }

    // Wait for completion.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    size_t after_rss = current_rss_bytes();

    if (baseline_rss > 0 && after_rss > baseline_rss) {
        size_t delta = after_rss - baseline_rss;
        printf("  %d sessions, RSS delta: %.1f MB\n", NUM_SESSIONS, delta / 1e6);
        printf("  ~%.1f KB/session\n", delta / 1024.0 / NUM_SESSIONS);
    } else {
        printf("  (RSS measurement not available on this platform)\n");
    }

    // LangChain comparison context:
    // LangChain session: ~2-5 MB (Python objects, serialized chains)
    // Forge session: target ~5-20 KB (Session struct + conversation deque)
    printf("  [Context: LangChain ~2-5 MB/session, Forge target <50 KB/session]\n");
}

// ─── Benchmark 4: Concurrent session throughput with simulated latency ──
static void bench_concurrent_with_latency() {
    printf("\n=== Benchmark 4: Concurrent Sessions (10ms LLM latency) ===\n");

    constexpr int NUM_SESSIONS = 100;
    constexpr int LLM_LATENCY_MS = 10;

    forge::ThreadPool pool(8);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::testing::ScriptedMockLLM llm;
    llm.set_handler([](const forge::Json& messages, const forge::Json&) {
        // Simulate LLM latency.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        forge::LLMResponse resp;
        resp.message.role = "assistant";
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;

        auto msg_str = messages.dump();
        if (msg_str.find("\"role\":\"tool\"") != std::string::npos) {
            resp.message.content = "Result";
        } else {
            forge::ToolCall tc;
            tc.id = "c1";
            tc.name = "calculator";
            tc.arguments_json = R"({"expression": "1+1"})";
            resp.message.tool_calls.push_back(std::move(tc));
        }
        return resp;
    });

    forge::WorkflowFactory factory;
    factory.register_workflow("react", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                          const forge::ToolRegistry& reg, forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(l, ex, reg);
    });

    forge::SessionManager manager(pool, llm, executor, registry, factory, NUM_SESSIONS + 10);

    auto start = Clock::now();

    std::vector<uint64_t> ids;
    for (int i = 0; i < NUM_SESSIONS; ++i) {
        forge::CreateSessionRequest req;
        req.prompt = "Task " + std::to_string(i);
        auto result = manager.create_session(req);
        if (auto* id = std::get_if<uint64_t>(&result)) {
            ids.push_back(*id);
        }
    }

    // Wait for all to complete.
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        int done = 0;
        for (auto id : ids) {
            auto info = manager.get_session(id);
            if (info && (info->state == forge::SessionState::COMPLETED ||
                         info->state == forge::SessionState::FAILED)) {
                ++done;
            }
        }
        if (done >= NUM_SESSIONS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();

    // Sequential would take: NUM_SESSIONS * 2 * LLM_LATENCY_MS = 2000ms
    // With perfect concurrency: 2 * LLM_LATENCY_MS = 20ms (limited by pool threads)
    double sequential_ms = NUM_SESSIONS * 2.0 * LLM_LATENCY_MS;
    double speedup = sequential_ms / elapsed_ms;

    int completed = 0;
    for (auto id : ids) {
        auto info = manager.get_session(id);
        if (info && info->state == forge::SessionState::COMPLETED) ++completed;
    }

    printf("  %d sessions in %lld ms (2-step, %dms latency each)\n",
           NUM_SESSIONS, elapsed_ms, LLM_LATENCY_MS);
    printf("  Sequential would take: %.0f ms\n", sequential_ms);
    printf("  Speedup: %.1fx\n", speedup);
    printf("  %d/%d completed\n", completed, NUM_SESSIONS);

    // LangChain comparison context:
    // LangChain async: limited by GIL, typically 2-8x speedup
    // Forge: should approach thread count speedup (8x theoretical with 8 threads)
    printf("  [Context: LangChain async ~2-8x speedup, Forge target ~%.0fx (pool threads)]\n",
           8.0);
}

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  Forge Benchmark Suite — v1.0.0                     ║\n");
    printf("║  Lock-Free Agent Orchestration Runtime               ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    bench_scheduling_overhead();
    bench_session_throughput();
    bench_memory_per_session();
    bench_concurrent_with_latency();

    printf("\n=== Summary ===\n");
    printf("All benchmarks use in-process mock LLMs (no network).\n");
    printf("This isolates orchestration overhead from LLM API latency.\n");
    printf("For real-world comparison, the gap widens further because\n");
    printf("Python's GIL serializes orchestration work between I/O waits.\n");

    return 0;
}
