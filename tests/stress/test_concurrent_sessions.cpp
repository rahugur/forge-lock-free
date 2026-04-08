// Phase 2 milestone test: 100 concurrent sessions.
// Verifies all sessions complete without contention, deadlock, or data corruption.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/rate_limiter.h"
#include "core/semaphore.h"
#include "core/thread_pool.h"
#include "llm/llm_client.h"
#include "llm/throttled_llm_client.h"
#include "session/session_manager.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/react.h"
#include "workflow/workflow_factory.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Mock LLM: first call returns a tool_call, second returns final answer.
/// Simulates a realistic 2-step ReAct loop.
struct TwoStepMockLLM {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::atomic<int> total_calls{0};

    TwoStepMockLLM() {
        // Use per-request logic: if the request contains a tool result
        // message, return the final answer. Otherwise, return a tool call.
        svr.Post("/v1/chat/completions",
            [this](const httplib::Request& req, httplib::Response& res) {
                total_calls.fetch_add(1);

                // Check if the request contains tool result messages.
                bool has_tool_result = req.body.find("\"role\":\"tool\"") !=
                                      std::string::npos;

                if (has_tool_result) {
                    // Second call: return final answer.
                    res.set_content(R"({
                        "choices": [{
                            "message": {"role":"assistant","content":"Done."}
                        }],
                        "usage": {"prompt_tokens":20,"completion_tokens":3}
                    })", "application/json");
                } else {
                    // First call: return tool call.
                    res.set_content(R"({
                        "choices": [{
                            "message": {
                                "role": "assistant",
                                "content": null,
                                "tool_calls": [{
                                    "id": "call_1",
                                    "type": "function",
                                    "function": {
                                        "name": "calculator",
                                        "arguments": "{\"expression\": \"1+1\"}"
                                    }
                                }]
                            }
                        }],
                        "usage": {"prompt_tokens":10,"completion_tokens":5}
                    })", "application/json");
                }
            });

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~TwoStepMockLLM() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

forge::WorkflowFactory make_factory() {
    forge::WorkflowFactory f;
    f.register_workflow("react", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                    const forge::ToolRegistry& reg, forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(l, ex, reg);
    });
    return f;
}

}  // namespace

TEST_CASE("Milestone: 100 concurrent sessions complete successfully",
          "[concurrent][milestone]") {
    constexpr int NUM_SESSIONS = 100;

    TwoStepMockLLM mock;

    forge::ThreadPool pool(8);
    forge::AsyncHttpClient http(pool, {10, 60, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient raw_llm(http, llm_cfg);

    // Throttle: 20 concurrent LLM calls, high rate limit.
    forge::Semaphore sem(20);
    forge::RateLimiter rate(10000.0, 100);
    forge::ThrottledLLMClient llm(raw_llm, sem, rate);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, NUM_SESSIONS);

    // Create all sessions.
    std::vector<uint64_t> ids;
    ids.reserve(NUM_SESSIONS);

    for (int i = 0; i < NUM_SESSIONS; ++i) {
        forge::CreateSessionRequest req;
        req.prompt = "Session " + std::to_string(i) + ": compute 1+1";

        auto result = manager.create_session(req);
        REQUIRE(std::holds_alternative<uint64_t>(result));
        ids.push_back(std::get<uint64_t>(result));
    }

    REQUIRE(ids.size() == NUM_SESSIONS);

    // Wait for all to complete (with timeout).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int completed = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        completed = 0;
        for (auto id : ids) {
            auto info = manager.get_session(id);
            REQUIRE(info.has_value());
            if (info->state == forge::SessionState::COMPLETED ||
                info->state == forge::SessionState::FAILED) {
                ++completed;
            }
        }
        if (completed == NUM_SESSIONS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify all sessions completed successfully.
    int success_count = 0;
    int fail_count = 0;

    for (auto id : ids) {
        auto info = manager.get_session(id);
        REQUIRE(info.has_value());
        if (info->state == forge::SessionState::COMPLETED) {
            REQUIRE(info->final_answer == "Done.");
            ++success_count;
        } else {
            ++fail_count;
        }
    }

    REQUIRE(success_count == NUM_SESSIONS);
    REQUIRE(fail_count == 0);

    // Each session makes 2 LLM calls.
    REQUIRE(mock.total_calls.load() == NUM_SESSIONS * 2);
}

TEST_CASE("Backpressure: concurrent LLM calls respect semaphore",
          "[concurrent][backpressure]") {
    constexpr int NUM_SESSIONS = 30;
    constexpr int MAX_CONCURRENT = 5;

    // Mock LLM with a concurrency gauge.
    httplib::Server svr;
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<bool> violation{false};

    svr.Post("/v1/chat/completions",
        [&](const httplib::Request& req, httplib::Response& res) {
            int cur = in_flight.fetch_add(1) + 1;
            if (cur > MAX_CONCURRENT) {
                violation.store(true, std::memory_order_relaxed);
            }
            // Track max.
            int prev = max_in_flight.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !max_in_flight.compare_exchange_weak(prev, cur)) {}

            // Simulate some latency.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            in_flight.fetch_sub(1);

            // Check for tool results to determine step.
            bool has_tool = req.body.find("\"role\":\"tool\"") !=
                            std::string::npos;

            if (has_tool) {
                res.set_content(R"({
                    "choices":[{"message":{"role":"assistant","content":"OK"}}],
                    "usage":{"prompt_tokens":10,"completion_tokens":3}
                })", "application/json");
            } else {
                res.set_content(R"({
                    "choices":[{"message":{"role":"assistant","content":null,
                        "tool_calls":[{"id":"c1","type":"function",
                            "function":{"name":"calculator",
                                "arguments":"{\"expression\":\"1+1\"}"}}]
                    }}],
                    "usage":{"prompt_tokens":10,"completion_tokens":5}
                })", "application/json");
            }
        });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&svr]() { svr.listen_after_bind(); });

    forge::ThreadPool pool(8);
    forge::AsyncHttpClient http(pool, {10, 60, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = "http://127.0.0.1:" + std::to_string(port);
    llm_cfg.api_key = "test";
    forge::LLMClient raw_llm(http, llm_cfg);

    forge::Semaphore sem(MAX_CONCURRENT);
    forge::RateLimiter rate(100000.0, 1000);  // very high rate limit
    forge::ThrottledLLMClient llm(raw_llm, sem, rate);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, NUM_SESSIONS);

    for (int i = 0; i < NUM_SESSIONS; ++i) {
        forge::CreateSessionRequest req;
        req.prompt = "test " + std::to_string(i);
        manager.create_session(req);
    }

    // Wait for completion.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (manager.active_count() == 0) break;

        int done = 0;
        for (auto id : manager.list_sessions()) {
            auto info = manager.get_session(id);
            if (info && info->state == forge::SessionState::COMPLETED) ++done;
        }
        if (done >= NUM_SESSIONS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // The semaphore should have prevented more than MAX_CONCURRENT in-flight.
    REQUIRE_FALSE(violation.load());
    REQUIRE(max_in_flight.load() <= MAX_CONCURRENT);

    svr.stop();
    server_thread.join();
}
