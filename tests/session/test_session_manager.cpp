// Tests for the concurrent session manager.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/thread_pool.h"
#include "llm/llm_client.h"
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

namespace {

/// Mock LLM server that always returns an immediate final answer.
struct QuickMockLLM {
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    QuickMockLLM() {
        svr.Post("/v1/chat/completions",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "choices": [{
                        "message": {
                            "role": "assistant",
                            "content": "The answer is 42."
                        }
                    }],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 5}
                })", "application/json");
            });

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~QuickMockLLM() {
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

TEST_CASE("SessionManager: create and complete", "[session_manager]") {
    QuickMockLLM mock;

    forge::ThreadPool pool(4);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    forge::CreateSessionRequest req;
    req.prompt = "What is the meaning of life?";

    auto result = manager.create_session(req);
    REQUIRE(std::holds_alternative<uint64_t>(result));
    auto id = std::get<uint64_t>(result);
    REQUIRE(id >= 1);

    // Wait for completion.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto info = manager.get_session(id);
        REQUIRE(info.has_value());
        if (info->state == forge::SessionState::COMPLETED) {
            REQUIRE(info->final_answer == "The answer is 42.");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto info = manager.get_session(id);
    REQUIRE(info.has_value());
    REQUIRE(info->state == forge::SessionState::COMPLETED);
}

TEST_CASE("SessionManager: incrementing IDs", "[session_manager]") {
    QuickMockLLM mock;

    forge::ThreadPool pool(8);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    forge::CreateSessionRequest req;
    req.prompt = "test";

    auto r1 = manager.create_session(req);
    auto r2 = manager.create_session(req);
    auto r3 = manager.create_session(req);

    REQUIRE(std::holds_alternative<uint64_t>(r1));
    REQUIRE(std::holds_alternative<uint64_t>(r2));
    REQUIRE(std::holds_alternative<uint64_t>(r3));

    auto id1 = std::get<uint64_t>(r1);
    auto id2 = std::get<uint64_t>(r2);
    auto id3 = std::get<uint64_t>(r3);

    REQUIRE(id2 == id1 + 1);
    REQUIRE(id3 == id2 + 1);

    // Wait for all to complete.
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

TEST_CASE("SessionManager: capacity enforcement", "[session_manager]") {
    QuickMockLLM mock;

    forge::ThreadPool pool(8);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    // Max 2 sessions.
    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 2);

    forge::CreateSessionRequest req;
    req.prompt = "test";

    auto r1 = manager.create_session(req);
    auto r2 = manager.create_session(req);
    REQUIRE(std::holds_alternative<uint64_t>(r1));
    REQUIRE(std::holds_alternative<uint64_t>(r2));

    // Third should fail (before GC runs).
    auto r3 = manager.create_session(req);
    REQUIRE(std::holds_alternative<std::string>(r3));

    std::this_thread::sleep_for(std::chrono::seconds(2));
}

TEST_CASE("SessionManager: cancel session", "[session_manager]") {
    // Mock that takes a while (returns tool calls to slow things down).
    httplib::Server svr;
    std::atomic<int> call_count{0};
    svr.Post("/v1/chat/completions",
        [&call_count](const httplib::Request&, httplib::Response& res) {
            // Always return tool call to keep the loop going.
            int n = call_count.fetch_add(1);
            if (n == 0) {
                // First: add a small delay, then return tool call.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
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
                "usage": {"prompt_tokens": 10, "completion_tokens": 5}
            })", "application/json");
        });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&svr]() { svr.listen_after_bind(); });

    forge::ThreadPool pool(4);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = "http://127.0.0.1:" + std::to_string(port);
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    forge::CreateSessionRequest req;
    req.prompt = "loop";
    req.config.max_iterations = 100;  // high limit so cancellation must stop it

    auto result = manager.create_session(req);
    REQUIRE(std::holds_alternative<uint64_t>(result));
    auto id = std::get<uint64_t>(result);

    // Wait for the session to start running.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Cancel it.
    REQUIRE(manager.cancel_session(id));

    // Wait for cancellation to take effect.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto info = manager.get_session(id);
        if (info && info->state == forge::SessionState::CANCELLED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto info = manager.get_session(id);
    REQUIRE(info.has_value());
    REQUIRE(info->state == forge::SessionState::CANCELLED);

    svr.stop();
    server_thread.join();
}

TEST_CASE("SessionManager: GC reaps terminal sessions", "[session_manager]") {
    QuickMockLLM mock;

    forge::ThreadPool pool(4);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    forge::CreateSessionRequest req;
    req.prompt = "test";
    manager.create_session(req);
    manager.create_session(req);

    // Wait for completion.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    REQUIRE(manager.active_count() == 2);

    // GC with 0 age should reap all terminal sessions.
    size_t reaped = manager.gc(std::chrono::seconds(0));
    REQUIRE(reaped == 2);
    REQUIRE(manager.active_count() == 0);
}

TEST_CASE("SessionManager: unknown session returns nullopt", "[session_manager]") {
    QuickMockLLM mock;

    forge::ThreadPool pool(8);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    REQUIRE_FALSE(manager.get_session(9999).has_value());
    REQUIRE_FALSE(manager.cancel_session(9999));
}
