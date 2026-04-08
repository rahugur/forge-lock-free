#pragma once
// HTTP REST API server for session management.
// Uses cpp-httplib (already a dependency).

#include "session/session_manager.h"

#include <httplib.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace forge {

class HttpServer {
public:
    HttpServer(SessionManager& manager, const std::string& host, uint16_t port);
    ~HttpServer();

    /// Start listening (blocks until stop() is called).
    void start();

    /// Stop the server (thread-safe).
    void stop();

private:
    void setup_routes();

    // Route handlers.
    void handle_create_session(const httplib::Request& req, httplib::Response& res);
    void handle_get_session(const httplib::Request& req, httplib::Response& res);
    void handle_delete_session(const httplib::Request& req, httplib::Response& res);
    void handle_list_sessions(const httplib::Request& req, httplib::Response& res);
    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_metrics(const httplib::Request& req, httplib::Response& res);
    void handle_subscribe(const httplib::Request& req, httplib::Response& res);

    SessionManager& manager_;
    httplib::Server svr_;
    std::string host_;
    uint16_t port_;

    // GC background thread.
    std::thread gc_thread_;
    std::atomic<bool> gc_stop_{false};
};

}  // namespace forge
