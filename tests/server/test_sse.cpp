// Tests for SSE (Server-Sent Events) session subscription.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/thread_pool.h"
#include "llm/llm_client.h"
#include "server/http_server.h"
#include "session/session_manager.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/react.h"
#include "workflow/workflow_factory.h"

#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

struct MockLLM {
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    MockLLM() {
        svr.Post("/v1/chat/completions",
            [](const httplib::Request& req, httplib::Response& res) {
                bool has_tool = req.body.find("\"role\":\"tool\"") != std::string::npos;
                if (has_tool) {
                    res.set_content(R"({
                        "choices":[{"message":{"role":"assistant","content":"42"}}],
                        "usage":{"prompt_tokens":10,"completion_tokens":5}
                    })", "application/json");
                } else {
                    res.set_content(R"({
                        "choices":[{"message":{"role":"assistant","content":null,
                            "tool_calls":[{"id":"c1","type":"function",
                                "function":{"name":"calculator",
                                    "arguments":"{\"expression\":\"6*7\"}"}}]
                        }}],
                        "usage":{"prompt_tokens":10,"completion_tokens":5}
                    })", "application/json");
                }
            });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~MockLLM() {
        svr.stop();
        if (thread.joinable()) thread.join();
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

TEST_CASE("SSE: subscribe receives state events", "[sse]") {
    MockLLM mock;
    forge::ThreadPool pool(4);
    forge::AsyncHttpClient http(pool, {10, 30, false});

    forge::LLMClientConfig llm_cfg;
    llm_cfg.api_base = "http://127.0.0.1:" + std::to_string(mock.port);
    llm_cfg.api_key = "test";
    forge::LLMClient llm(http, llm_cfg);

    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    auto factory = make_factory();
    forge::SessionManager manager(pool, llm, executor, registry, factory, 100);

    // Set up a raw httplib server that proxies to our handler.
    httplib::Server raw_svr;

    // POST /api/sessions — create session via manager.
    raw_svr.Post("/api/sessions",
        [&](const httplib::Request& req, httplib::Response& res) {
            auto j = forge::Json::parse(req.body);
            forge::CreateSessionRequest create_req;
            create_req.prompt = j.value("prompt", "");
            auto result = manager.create_session(std::move(create_req));
            if (auto* id = std::get_if<uint64_t>(&result)) {
                forge::Json resp;
                resp["id"] = *id;
                res.status = 201;
                res.set_content(resp.dump(), "application/json");
            }
        });

    // GET /api/sessions/:id/subscribe — SSE stream.
    raw_svr.Get(R"(/api/sessions/(\d+)/subscribe)",
        [&](const httplib::Request& req, httplib::Response& res) {
            uint64_t id = std::stoull(req.matches[1].str());

            auto initial = manager.get_session(id);
            if (!initial) {
                res.status = 404;
                res.set_content(R"({"error":"Not found"})", "application/json");
                return;
            }

            res.set_header("Cache-Control", "no-cache");

            auto last_state = std::make_shared<forge::SessionState>(
                forge::SessionState::CREATED);
            auto mgr = &manager;

            res.set_chunked_content_provider(
                "text/event-stream",
                [mgr, id, last_state](size_t, httplib::DataSink& sink) -> bool {
                    auto info = mgr->get_session(id);
                    if (!info) {
                        sink.done();
                        return false;
                    }

                    if (info->state != *last_state) {
                        *last_state = info->state;

                        forge::Json data;
                        data["state"] = forge::session_state_name(info->state);
                        data["step_count"] = info->step_count;
                        if (!info->final_answer.empty())
                            data["answer"] = info->final_answer;

                        std::string event = "event: state\ndata: " +
                                            data.dump() + "\n\n";
                        sink.write(event.c_str(), event.size());

                        if (info->state == forge::SessionState::COMPLETED ||
                            info->state == forge::SessionState::FAILED) {
                            std::string done = "event: done\ndata: {}\n\n";
                            sink.write(done.c_str(), done.size());
                            sink.done();
                            return false;
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    return true;
                }
            );
        });

    int port = raw_svr.bind_to_any_port("127.0.0.1");
    std::thread svr_thread([&]() { raw_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);

    // Create a session.
    auto create_res = cli.Post("/api/sessions",
        R"({"prompt":"Compute 6*7"})", "application/json");
    REQUIRE(create_res);
    REQUIRE(create_res->status == 201);
    auto session_id = forge::Json::parse(create_res->body)["id"].get<uint64_t>();

    // Subscribe to SSE events.
    std::string sse_body;
    bool got_completed = false;

    // Use a thread to read SSE since it blocks.
    std::thread sse_thread([&]() {
        httplib::Client sse_cli("127.0.0.1", port);
        sse_cli.set_read_timeout(10, 0);
        auto res = sse_cli.Get(
            "/api/sessions/" + std::to_string(session_id) + "/subscribe");
        if (res) {
            sse_body = res->body;
            got_completed = sse_body.find("\"completed\"") != std::string::npos;
        }
    });

    // Wait for SSE thread to finish (session should complete quickly).
    sse_thread.join();

    // Verify we got SSE events.
    REQUIRE_FALSE(sse_body.empty());
    REQUIRE(sse_body.find("event: state") != std::string::npos);
    REQUIRE(got_completed);
    REQUIRE(sse_body.find("event: done") != std::string::npos);

    raw_svr.stop();
    svr_thread.join();
}

TEST_CASE("SSE: subscribe to nonexistent session returns 404", "[sse]") {
    httplib::Server raw_svr;
    raw_svr.Get(R"(/api/sessions/(\d+)/subscribe)",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 404;
            res.set_content(R"({"error":"Not found"})", "application/json");
        });

    int port = raw_svr.bind_to_any_port("127.0.0.1");
    std::thread svr_thread([&]() { raw_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/sessions/9999/subscribe");
    REQUIRE(res);
    REQUIRE(res->status == 404);

    raw_svr.stop();
    svr_thread.join();
}
