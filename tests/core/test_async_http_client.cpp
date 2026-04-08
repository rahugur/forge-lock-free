// Tests for the async HTTP client.
// Spins up a local httplib server to avoid network dependency.
#include <catch2/catch_test_macros.hpp>

#include "core/async_http_client.h"
#include "core/thread_pool.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

// Lightweight local HTTP server for tests.
struct LocalServer {
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    LocalServer() {
        svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("pong", "text/plain");
        });

        svr.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(req.body, req.get_header_value("Content-Type"));
        });

        svr.Get("/slow", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            res.set_content("done", "text/plain");
        });

        // Bind to any free port.
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { svr.listen_after_bind(); });
    }

    ~LocalServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

}  // namespace

TEST_CASE("AsyncHttpClient: GET request", "[http]") {
    LocalServer server;
    forge::ThreadPool pool(2);
    // Disable SSRF guard for localhost tests.
    forge::AsyncHttpClient client(pool, {10, 30, /*block_private_ips=*/false});

    auto fut = client.get(server.url("/ping"));
    auto resp = fut.get();

    REQUIRE(resp.error.empty());
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "pong");
}

TEST_CASE("AsyncHttpClient: POST request", "[http]") {
    LocalServer server;
    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    auto fut = client.post(server.url("/echo"), R"({"key":"value"})", "application/json");
    auto resp = fut.get();

    REQUIRE(resp.error.empty());
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == R"({"key":"value"})");
}

TEST_CASE("AsyncHttpClient: 404 response", "[http]") {
    LocalServer server;
    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    auto fut = client.get(server.url("/nonexistent"));
    auto resp = fut.get();

    REQUIRE(resp.error.empty());
    REQUIRE(resp.status == 404);
}

TEST_CASE("AsyncHttpClient: concurrent requests", "[http][stress]") {
    LocalServer server;
    forge::ThreadPool pool(4);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    constexpr int N = 20;
    std::vector<forge::Future<forge::HttpResponse>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i) {
        futures.push_back(client.get(server.url("/ping")));
    }

    for (auto& f : futures) {
        auto resp = f.get();
        REQUIRE(resp.status == 200);
        REQUIRE(resp.body == "pong");
    }
}

TEST_CASE("AsyncHttpClient: timeout via wait_for", "[http]") {
    LocalServer server;
    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    auto fut = client.get(server.url("/slow"));

    // Should timeout quickly — the /slow endpoint sleeps 3s.
    auto result = fut.wait_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AsyncHttpClient: connection refused", "[http]") {
    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    // Port 1 is almost certainly not listening.
    auto fut = client.get("http://127.0.0.1:1/nope");
    auto resp = fut.get();

    REQUIRE_FALSE(resp.error.empty());
    REQUIRE(resp.status == 0);
}

TEST_CASE("AsyncHttpClient: SSRF blocks private IPs", "[http][security]") {
    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool);  // SSRF enabled by default

    // Should throw on private IPs.
    auto fut = client.get("http://169.254.169.254/latest/meta-data/");
    REQUIRE_THROWS_AS(fut.get(), std::invalid_argument);

    auto fut2 = client.get("http://127.0.0.1:8080/admin");
    REQUIRE_THROWS_AS(fut2.get(), std::invalid_argument);

    auto fut3 = client.get("http://10.0.0.1/internal");
    REQUIRE_THROWS_AS(fut3.get(), std::invalid_argument);

    auto fut4 = client.get("http://localhost/admin");
    REQUIRE_THROWS_AS(fut4.get(), std::invalid_argument);
}

TEST_CASE("AsyncHttpClient: custom headers", "[http]") {
    httplib::Server svr;
    svr.Get("/check-header", [](const httplib::Request& req, httplib::Response& res) {
        auto it = req.headers.find("X-Custom");
        if (it != req.headers.end()) {
            res.set_content(it->second, "text/plain");
        } else {
            res.status = 400;
        }
    });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&svr]() { svr.listen_after_bind(); });

    forge::ThreadPool pool(2);
    forge::AsyncHttpClient client(pool, {10, 30, false});

    auto fut = client.get(
        "http://127.0.0.1:" + std::to_string(port) + "/check-header",
        {{"X-Custom", "hello-forge"}});
    auto resp = fut.get();

    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "hello-forge");

    svr.stop();
    t.join();
}
