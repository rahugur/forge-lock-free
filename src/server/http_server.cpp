#include "server/http_server.h"
#include "utils/json.h"
#include "utils/metrics.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace forge {

HttpServer::HttpServer(SessionManager& manager,
                       const std::string& host, uint16_t port)
    : manager_(manager), host_(host), port_(port) {
    setup_routes();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    // Start GC background thread.
    gc_thread_ = std::thread([this]() {
        while (!gc_stop_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (!gc_stop_.load(std::memory_order_acquire)) {
                manager_.gc(std::chrono::seconds(600));
            }
        }
    });

    spdlog::info("HTTP server starting on {}:{}", host_, port_);
    svr_.listen(host_, port_);
}

void HttpServer::stop() {
    svr_.stop();

    gc_stop_.store(true, std::memory_order_release);
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

void HttpServer::setup_routes() {
    // POST /api/sessions — create a new session.
    svr_.Post("/api/sessions",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_create_session(req, res);
        });

    // GET /api/sessions — list all sessions.
    svr_.Get("/api/sessions",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_list_sessions(req, res);
        });

    // GET /api/sessions/:id
    svr_.Get(R"(/api/sessions/(\d+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_get_session(req, res);
        });

    // DELETE /api/sessions/:id
    svr_.Delete(R"(/api/sessions/(\d+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_delete_session(req, res);
        });

    // GET /health
    svr_.Get("/health",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_health(req, res);
        });

    // GET /api/metrics
    svr_.Get("/api/metrics",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_metrics(req, res);
        });

    // GET /api/sessions/:id/subscribe — SSE stream of session events.
    svr_.Get(R"(/api/sessions/(\d+)/subscribe)",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_subscribe(req, res);
        });
}

void HttpServer::handle_create_session(const httplib::Request& req,
                                        httplib::Response& res) {
    try {
        auto j = Json::parse(req.body);

        CreateSessionRequest create_req;
        create_req.prompt = j.value("prompt", "");

        if (create_req.prompt.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"Missing 'prompt' field"})",
                            "application/json");
            return;
        }

        create_req.system_message = j.value("system_message",
            "You are a helpful assistant.");
        create_req.workflow = j.value("workflow", "react");

        if (j.contains("tools") && j["tools"].is_array()) {
            for (auto& t : j["tools"]) {
                create_req.tools.push_back(t.get<std::string>());
            }
        }

        if (j.contains("max_iterations")) {
            create_req.config.max_iterations = j["max_iterations"].get<unsigned>();
        }
        if (j.contains("timeout_seconds")) {
            create_req.config.timeout =
                std::chrono::seconds(j["timeout_seconds"].get<unsigned>());
        }

        auto result = manager_.create_session(std::move(create_req));

        if (auto* id = std::get_if<uint64_t>(&result)) {
            Json resp;
            resp["id"] = *id;
            res.status = 201;
            res.set_content(resp.dump(), "application/json");
        } else {
            auto& err = std::get<std::string>(result);
            Json resp;
            resp["error"] = err;
            res.status = 503;
            res.set_content(resp.dump(), "application/json");
        }
    } catch (const std::exception& e) {
        Json resp;
        resp["error"] = std::string("Invalid request: ") + e.what();
        res.status = 400;
        res.set_content(resp.dump(), "application/json");
    }
}

void HttpServer::handle_get_session(const httplib::Request& req,
                                     httplib::Response& res) {
    try {
        uint64_t id = std::stoull(req.matches[1].str());
        auto info = manager_.get_session(id);

        if (!info) {
            res.status = 404;
            res.set_content(R"({"error":"Session not found"})",
                            "application/json");
            return;
        }

        Json resp;
        resp["id"] = info->id;
        resp["state"] = session_state_name(info->state);
        resp["step_count"] = info->step_count;
        resp["answer"] = info->final_answer;
        resp["error"] = info->error;
        res.set_content(resp.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        Json resp;
        resp["error"] = std::string("Invalid session ID: ") + e.what();
        res.set_content(resp.dump(), "application/json");
    }
}

void HttpServer::handle_delete_session(const httplib::Request& req,
                                        httplib::Response& res) {
    try {
        uint64_t id = std::stoull(req.matches[1].str());
        bool cancelled = manager_.cancel_session(id);

        if (!cancelled) {
            res.status = 404;
            res.set_content(R"({"error":"Session not found"})",
                            "application/json");
            return;
        }

        Json resp;
        resp["cancelled"] = true;
        res.set_content(resp.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        Json resp;
        resp["error"] = std::string("Invalid session ID: ") + e.what();
        res.set_content(resp.dump(), "application/json");
    }
}

void HttpServer::handle_list_sessions(const httplib::Request&,
                                       httplib::Response& res) {
    auto ids = manager_.list_sessions();

    Json sessions = Json::array();
    for (auto id : ids) {
        auto info = manager_.get_session(id);
        if (info) {
            Json s;
            s["id"] = info->id;
            s["state"] = session_state_name(info->state);
            s["step_count"] = info->step_count;
            sessions.push_back(std::move(s));
        }
    }

    Json resp;
    resp["sessions"] = sessions;
    res.set_content(resp.dump(), "application/json");
}

void HttpServer::handle_health(const httplib::Request&,
                                httplib::Response& res) {
    Json resp;
    resp["status"] = "ok";
    resp["active_sessions"] = manager_.active_count();
    res.set_content(resp.dump(), "application/json");
}

void HttpServer::handle_metrics(const httplib::Request&,
                                 httplib::Response& res) {
    auto j = Metrics::instance().to_json();
    j["active_sessions"] = manager_.active_count();
    res.set_content(j.dump(), "application/json");
}

void HttpServer::handle_subscribe(const httplib::Request& req,
                                   httplib::Response& res) {
    uint64_t id = 0;
    try {
        id = std::stoull(req.matches[1].str());
    } catch (...) {
        res.status = 400;
        res.set_content(R"({"error":"Invalid session ID"})", "application/json");
        return;
    }

    // Verify session exists.
    auto initial = manager_.get_session(id);
    if (!initial) {
        res.status = 404;
        res.set_content(R"({"error":"Session not found"})", "application/json");
        return;
    }

    // Set SSE headers.
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    // Use chunked content provider to stream SSE events.
    // We poll the session state and emit events on changes.
    auto last_state = std::make_shared<SessionState>(SessionState::CREATED);
    auto last_step = std::make_shared<uint32_t>(0);
    auto mgr = &manager_;
    auto session_id = id;

    res.set_chunked_content_provider(
        "text/event-stream",
        [mgr, session_id, last_state, last_step](size_t /*offset*/,
                                                   httplib::DataSink& sink) -> bool {
            auto info = mgr->get_session(session_id);
            if (!info) {
                // Session was GC'd.
                std::string event = "event: error\ndata: {\"error\":\"Session not found\"}\n\n";
                sink.write(event.c_str(), event.size());
                sink.done();
                return false;
            }

            bool changed = (info->state != *last_state) ||
                           (info->step_count != *last_step);

            if (changed) {
                *last_state = info->state;
                *last_step = info->step_count;

                Json data;
                data["id"] = info->id;
                data["state"] = session_state_name(info->state);
                data["step_count"] = info->step_count;

                if (!info->final_answer.empty()) {
                    data["answer"] = info->final_answer;
                }
                if (!info->error.empty()) {
                    data["error"] = info->error;
                }

                std::string event = "event: state\ndata: " + data.dump() + "\n\n";
                if (!sink.write(event.c_str(), event.size())) {
                    return false;  // client disconnected
                }

                // If terminal, send done event and close.
                if (info->state == SessionState::COMPLETED ||
                    info->state == SessionState::FAILED ||
                    info->state == SessionState::CANCELLED) {
                    std::string done_event = "event: done\ndata: {}\n\n";
                    sink.write(done_event.c_str(), done_event.size());
                    sink.done();
                    return false;
                }
            }

            // Poll interval.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return true;
        }
    );
}

}  // namespace forge
