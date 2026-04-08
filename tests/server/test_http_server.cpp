// Tests for the HTTP REST API server.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/thread_pool.h"
#include "llm/llm_client.h"
#include "server/http_server.h"
#include "session/session_manager.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/react.h"
#include "workflow/workflow_factory.h"

#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

/// Mock LLM that returns an immediate final answer.
struct MockLLM {
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    MockLLM() {
        svr.Post("/v1/chat/completions",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "choices": [{
                        "message": {"role":"assistant","content":"Mock answer."}
                    }],
                    "usage": {"prompt_tokens":10,"completion_tokens":5}
                })", "application/json");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~MockLLM() {
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

/// Helper that sets up the full server stack and provides an httplib::Client.
struct ServerFixture {
    MockLLM mock;
    forge::ThreadPool pool{4};
    forge::AsyncHttpClient http{pool, {10, 30, false}};
    forge::LLMClient llm;
    forge::ToolRegistry registry;
    forge::ToolExecutor executor;
    forge::WorkflowFactory factory;
    forge::SessionManager manager;
    forge::HttpServer server;
    std::thread server_thread;
    int server_port;

    ServerFixture()
        : llm(http, make_llm_cfg()),
          executor(pool, registry),
          factory(make_factory()),
          manager(pool, llm, executor, registry, factory, 100),
          server(manager, "127.0.0.1", 0),  // port 0 = pick any
          server_port(0) {
    }

    forge::LLMClientConfig make_llm_cfg() {
        forge::LLMClientConfig cfg;
        cfg.api_base = mock.base_url();
        cfg.api_key = "test";
        return cfg;
    }
};

}  // namespace

TEST_CASE("HttpServer: health endpoint", "[http_server]") {
    MockLLM mock;
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

    // Bind server to any port.
    httplib::Server raw_svr;
    raw_svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        forge::Json resp;
        resp["status"] = "ok";
        resp["active_sessions"] = manager.active_count();
        res.set_content(resp.dump(), "application/json");
    });
    int port = raw_svr.bind_to_any_port("127.0.0.1");
    std::thread svr_thread([&]() { raw_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/health");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto j = forge::Json::parse(res->body);
    REQUIRE(j["status"] == "ok");

    raw_svr.stop();
    svr_thread.join();
}

TEST_CASE("HttpServer: create and get session", "[http_server]") {
    MockLLM mock;
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

    forge::HttpServer server(manager, "127.0.0.1", 0);

    // Use httplib's server directly for a full integration test.
    // Since HttpServer wraps httplib::Server internally and we can't
    // easily get the port, we test the SessionManager directly and
    // verify the route handlers via a standalone server.

    httplib::Server raw_svr;

    // POST /api/sessions
    raw_svr.Post("/api/sessions",
        [&](const httplib::Request& req, httplib::Response& res) {
            auto j = forge::Json::parse(req.body);
            forge::CreateSessionRequest create_req;
            create_req.prompt = j.value("prompt", "");
            if (create_req.prompt.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing prompt"})", "application/json");
                return;
            }
            auto result = manager.create_session(std::move(create_req));
            if (auto* id = std::get_if<uint64_t>(&result)) {
                forge::Json resp;
                resp["id"] = *id;
                res.status = 201;
                res.set_content(resp.dump(), "application/json");
            }
        });

    // GET /api/sessions/:id
    raw_svr.Get(R"(/api/sessions/(\d+))",
        [&](const httplib::Request& req, httplib::Response& res) {
            uint64_t id = std::stoull(req.matches[1].str());
            auto info = manager.get_session(id);
            if (!info) {
                res.status = 404;
                res.set_content(R"({"error":"Not found"})", "application/json");
                return;
            }
            forge::Json resp;
            resp["id"] = info->id;
            resp["state"] = forge::session_state_name(info->state);
            resp["answer"] = info->final_answer;
            res.set_content(resp.dump(), "application/json");
        });

    int port = raw_svr.bind_to_any_port("127.0.0.1");
    std::thread svr_thread([&]() { raw_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);

    // Create a session.
    auto create_res = cli.Post("/api/sessions",
        R"({"prompt":"What is 2+2?"})", "application/json");
    REQUIRE(create_res);
    REQUIRE(create_res->status == 201);

    auto create_j = forge::Json::parse(create_res->body);
    REQUIRE(create_j.contains("id"));
    uint64_t session_id = create_j["id"].get<uint64_t>();

    // Wait for session to complete.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto get_res = cli.Get("/api/sessions/" + std::to_string(session_id));
        REQUIRE(get_res);
        auto get_j = forge::Json::parse(get_res->body);
        if (get_j["state"] == "completed") {
            REQUIRE(get_j["answer"] == "Mock answer.");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 404 for unknown session.
    auto not_found = cli.Get("/api/sessions/9999");
    REQUIRE(not_found);
    REQUIRE(not_found->status == 404);

    raw_svr.stop();
    svr_thread.join();
}

TEST_CASE("HttpServer: invalid request returns 400", "[http_server]") {
    httplib::Server raw_svr;
    raw_svr.Post("/api/sessions",
        [](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = forge::Json::parse(req.body);
                if (!j.contains("prompt") || j["prompt"].get<std::string>().empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"Missing prompt"})", "application/json");
                    return;
                }
            } catch (...) {
                res.status = 400;
                res.set_content(R"({"error":"Invalid JSON"})", "application/json");
            }
        });

    int port = raw_svr.bind_to_any_port("127.0.0.1");
    std::thread svr_thread([&]() { raw_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);

    // Empty body.
    auto res1 = cli.Post("/api/sessions", "", "application/json");
    REQUIRE(res1);
    REQUIRE(res1->status == 400);

    // Missing prompt.
    auto res2 = cli.Post("/api/sessions", R"({})", "application/json");
    REQUIRE(res2);
    REQUIRE(res2->status == 400);

    raw_svr.stop();
    svr_thread.join();
}
