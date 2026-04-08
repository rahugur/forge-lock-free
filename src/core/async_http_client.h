#pragma once
// Async HTTP client for the Forge runtime.
//
// Phase 0: dispatches blocking cpp-httplib calls on the thread pool.
// Phase 1+: swap in libcurl multi or io_uring for true non-blocking I/O.

#include "core/future.h"
#include "core/thread_pool.h"

#include <httplib.h>

#include <string>
#include <unordered_map>
#include <stdexcept>
#include <vector>

namespace forge {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;  // non-empty on transport failure
};

struct HttpClientConfig {
    int connect_timeout_sec = 10;
    int read_timeout_sec = 30;
    /// If true, reject URLs targeting private/link-local IPs (SSRF protection).
    bool block_private_ips = true;
};

class AsyncHttpClient {
public:
    explicit AsyncHttpClient(ThreadPool& pool,
                             HttpClientConfig cfg = {})
        : pool_(pool), cfg_(cfg) {}

    /// Async GET.  Returns a future that resolves when the response arrives.
    Future<HttpResponse> get(const std::string& url,
                             const std::unordered_map<std::string, std::string>& hdrs = {}) {
        auto config = cfg_;
        return pool_.submit([url, hdrs, config]() -> HttpResponse {
            auto parts = parse_url(url);
            if (config.block_private_ips) {
                check_ssrf(extract_host(parts.scheme_host_port));
            }
            httplib::Client cli(parts.scheme_host_port);
            cli.set_connection_timeout(config.connect_timeout_sec);
            cli.set_read_timeout(config.read_timeout_sec);

            httplib::Headers h;
            for (auto& [k, v] : hdrs) h.emplace(k, v);

            auto res = cli.Get(parts.path, h);
            return to_response(res);
        });
    }

    /// Async POST with a body and content type.
    Future<HttpResponse> post(const std::string& url,
                              const std::string& body,
                              const std::string& content_type = "application/json",
                              const std::unordered_map<std::string, std::string>& hdrs = {}) {
        auto config = cfg_;
        return pool_.submit([url, body, content_type, hdrs, config]() -> HttpResponse {
            auto parts = parse_url(url);
            if (config.block_private_ips) {
                check_ssrf(extract_host(parts.scheme_host_port));
            }
            httplib::Client cli(parts.scheme_host_port);
            cli.set_connection_timeout(config.connect_timeout_sec);
            cli.set_read_timeout(config.read_timeout_sec);

            httplib::Headers h;
            for (auto& [k, v] : hdrs) h.emplace(k, v);

            auto res = cli.Post(parts.path, h, body, content_type);
            return to_response(res);
        });
    }

    /// Access the underlying thread pool (for chaining futures).
    ThreadPool& pool() { return pool_; }

private:
    struct UrlParts {
        std::string scheme_host_port;  // "http://host:port"
        std::string path;              // "/foo/bar"
    };

    /// Minimal URL parser: splits "http://host:port/path" into origin + path.
    static UrlParts parse_url(const std::string& url) {
        UrlParts parts;
        auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            throw std::invalid_argument("URL must include scheme: " + url);
        }
        auto path_start = url.find('/', scheme_end + 3);
        if (path_start == std::string::npos) {
            parts.scheme_host_port = url;
            parts.path = "/";
        } else {
            parts.scheme_host_port = url.substr(0, path_start);
            parts.path = url.substr(path_start);
        }
        return parts;
    }

    /// Extract the host portion from a scheme://host:port string.
    static std::string extract_host(const std::string& scheme_host_port) {
        auto scheme_end = scheme_host_port.find("://");
        std::string rest = (scheme_end != std::string::npos)
            ? scheme_host_port.substr(scheme_end + 3)
            : scheme_host_port;
        auto colon = rest.find(':');
        return (colon != std::string::npos) ? rest.substr(0, colon) : rest;
    }

    /// SSRF guard: reject URLs targeting private/link-local/metadata IPs.
    static void check_ssrf(const std::string& host) {
        // Block well-known private ranges by prefix.
        static const std::vector<std::string> blocked_prefixes = {
            "10.", "192.168.", "172.16.", "172.17.", "172.18.", "172.19.",
            "172.20.", "172.21.", "172.22.", "172.23.", "172.24.", "172.25.",
            "172.26.", "172.27.", "172.28.", "172.29.", "172.30.", "172.31.",
            "169.254.",  // link-local / cloud metadata
            "127.",      // loopback
            "0.",        // "this" network
        };
        for (auto& prefix : blocked_prefixes) {
            if (host.compare(0, prefix.size(), prefix) == 0) {
                throw std::invalid_argument(
                    "SSRF blocked: private/link-local IP " + host);
            }
        }
        if (host == "localhost" || host == "::1" || host.empty()) {
            throw std::invalid_argument(
                "SSRF blocked: loopback host " + host);
        }
    }

    static HttpResponse to_response(const httplib::Result& res) {
        HttpResponse resp;
        if (!res) {
            resp.error = httplib::to_string(res.error());
            return resp;
        }
        resp.status = res->status;
        resp.body = res->body;
        for (auto& [k, v] : res->headers) {
            resp.headers[k] = v;
        }
        return resp;
    }

    ThreadPool& pool_;
    HttpClientConfig cfg_;
};

}  // namespace forge
