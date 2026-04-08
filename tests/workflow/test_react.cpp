// Integration test: ReAct workflow with a mock LLM server.
// Verifies the full loop: LLM call → tool execution → observation → final answer.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/thread_pool.h"
#include "llm/llm_client.h"
#include "llm/message.h"
#include "session/session.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/react.h"

#include <httplib.h>

#include <atomic>
#include <string>
#include <thread>

namespace {

/// Mock OpenAI-compatible LLM server.
/// First call returns a tool_call for "calculator".
/// Second call returns the final answer incorporating the tool result.
struct MockLLMServer {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::atomic<int> call_count{0};

    MockLLMServer() {
        svr.Post("/v1/chat/completions",
            [this](const httplib::Request& req, httplib::Response& res) {
                int n = call_count.fetch_add(1);
                res.set_header("Content-Type", "application/json");

                if (n == 0) {
                    // First call: ask to use calculator.
                    res.set_content(R"({
                        "id": "chatcmpl-1",
                        "object": "chat.completion",
                        "model": "mock-model",
                        "choices": [{
                            "index": 0,
                            "message": {
                                "role": "assistant",
                                "content": null,
                                "tool_calls": [{
                                    "id": "call_abc123",
                                    "type": "function",
                                    "function": {
                                        "name": "calculator",
                                        "arguments": "{\"expression\": \"2^10 + 3*17\"}"
                                    }
                                }]
                            },
                            "finish_reason": "tool_calls"
                        }],
                        "usage": {"prompt_tokens": 50, "completion_tokens": 20}
                    })", "application/json");
                } else {
                    // Second call: return final answer.
                    res.set_content(R"({
                        "id": "chatcmpl-2",
                        "object": "chat.completion",
                        "model": "mock-model",
                        "choices": [{
                            "index": 0,
                            "message": {
                                "role": "assistant",
                                "content": "The answer is 1075."
                            },
                            "finish_reason": "stop"
                        }],
                        "usage": {"prompt_tokens": 100, "completion_tokens": 10}
                    })", "application/json");
                }
            });

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~MockLLMServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

}  // namespace

TEST_CASE("ReActWorkflow: full loop with mock LLM", "[react][integration]") {
    MockLLMServer mock;

    forge::ThreadPool pool(4);
    forge::AsyncHttpClient http(pool, {10, 30, /*block_private_ips=*/false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = mock.base_url();
    llm_cfg.model = "mock-model";
    llm_cfg.api_key = "test-key";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::SessionConfig session_cfg;
    session_cfg.max_iterations = 10;
    forge::Session session(1, session_cfg);

    forge::Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant.";
    session.history.push(std::move(sys));

    forge::Message user;
    user.role = "user";
    user.content = "What is 2^10 + 3*17?";
    session.history.push(std::move(user));

    forge::ReActWorkflow react(llm, executor, registry);
    auto result = react.run(session);

    REQUIRE(result.success);
    REQUIRE(result.answer == "The answer is 1075.");
    REQUIRE(result.steps_taken == 2);
    REQUIRE(result.total_prompt_tokens == 150);
    REQUIRE(result.total_completion_tokens == 30);
    REQUIRE(mock.call_count.load() == 2);
}

TEST_CASE("ReActWorkflow: max iterations guard", "[react]") {
    // Mock server that always returns tool calls (never a final answer).
    httplib::Server svr;
    svr.Post("/v1/chat/completions",
        [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": null,
                        "tool_calls": [{
                            "id": "call_loop",
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
    std::thread t([&svr]() { svr.listen_after_bind(); });

    forge::ThreadPool pool(2);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = "http://127.0.0.1:" + std::to_string(port);
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::SessionConfig session_cfg;
    session_cfg.max_iterations = 3;  // limit to 3 steps
    forge::Session session(2, session_cfg);

    forge::Message user;
    user.role = "user";
    user.content = "loop forever";
    session.history.push(std::move(user));

    forge::ReActWorkflow react(llm, executor, registry);
    auto result = react.run(session);

    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("exceeded") != std::string::npos);
    REQUIRE(result.steps_taken == 3);

    svr.stop();
    t.join();
}
