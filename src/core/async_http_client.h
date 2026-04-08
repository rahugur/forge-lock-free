#pragma once
// Async HTTP client for the Forge runtime.
//
// Phase 0: dispatches blocking cpp-httplib calls on the thread pool.
// Phase 1+: swap in libcurl multi or io_uring for true non-blocking I/O.

#include "core/future.h"
#include "core/thread_pool.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstring>
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
    /// Handles IPv6 brackets and strips any userinfo (user@) component.
    static std::string extract_host(const std::string& scheme_host_port) {
        auto scheme_end = scheme_host_port.find("://");
        std::string rest = (scheme_end != std::string::npos)
            ? scheme_host_port.substr(scheme_end + 3)
            : scheme_host_port;

        // Strip userinfo (anything before @) to prevent user@host bypass.
        auto at_pos = rest.find('@');
        if (at_pos != std::string::npos) {
            rest = rest.substr(at_pos + 1);
        }

        // Handle IPv6 bracket notation: [::1] or [::ffff:127.0.0.1]:port
        if (!rest.empty() && rest[0] == '[') {
            auto bracket_end = rest.find(']');
            if (bracket_end != std::string::npos) {
                return rest.substr(1, bracket_end - 1);
            }
        }

        auto colon = rest.find(':');
        return (colon != std::string::npos) ? rest.substr(0, colon) : rest;
    }

    /// Check if an IPv4 address (in network byte order) is private/loopback/link-local.
    static bool is_private_ipv4(uint32_t addr_net) {
        uint32_t addr = ntohl(addr_net);
        uint8_t a = (addr >> 24) & 0xFF;
        uint8_t b = (addr >> 16) & 0xFF;

        if (a == 10) return true;                              // 10.0.0.0/8
        if (a == 172 && b >= 16 && b <= 31) return true;       // 172.16.0.0/12
        if (a == 192 && b == 168) return true;                 // 192.168.0.0/16
        if (a == 127) return true;                             // 127.0.0.0/8
        if (a == 169 && b == 254) return true;                 // 169.254.0.0/16 (link-local/metadata)
        if (a == 0) return true;                               // 0.0.0.0/8
        return false;
    }

    /// Check if an IPv6 address is loopback, link-local, or IPv4-mapped private.
    static bool is_private_ipv6(const struct in6_addr& addr) {
        // ::1 (loopback)
        static const struct in6_addr loopback = IN6ADDR_LOOPBACK_INIT;
        if (memcmp(&addr, &loopback, sizeof(addr)) == 0) return true;

        // :: (unspecified)
        static const struct in6_addr any = IN6ADDR_ANY_INIT;
        if (memcmp(&addr, &any, sizeof(addr)) == 0) return true;

        // fe80::/10 (link-local)
        if (addr.s6_addr[0] == 0xfe && (addr.s6_addr[1] & 0xc0) == 0x80) return true;

        // fc00::/7 (unique local)
        if ((addr.s6_addr[0] & 0xfe) == 0xfc) return true;

        // ::ffff:0:0/96 (IPv4-mapped) -- check the embedded IPv4
        bool is_v4_mapped = true;
        for (int i = 0; i < 10; ++i) {
            if (addr.s6_addr[i] != 0) { is_v4_mapped = false; break; }
        }
        if (is_v4_mapped && addr.s6_addr[10] == 0xff && addr.s6_addr[11] == 0xff) {
            uint32_t v4;
            memcpy(&v4, &addr.s6_addr[12], 4);
            return is_private_ipv4(v4);
        }

        return false;
    }

    /// SSRF guard: resolve hostname to IP and reject private/loopback/link-local addresses.
    static void check_ssrf(const std::string& host) {
        if (host.empty()) {
            throw std::invalid_argument("SSRF blocked: empty host");
        }

        // First, try to parse as a literal IP address (handles octal, hex, etc.).
        struct in_addr v4;
        if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
            if (is_private_ipv4(v4.s_addr)) {
                throw std::invalid_argument("SSRF blocked: private IP " + host);
            }
            return;
        }

        struct in6_addr v6;
        if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
            if (is_private_ipv6(v6)) {
                throw std::invalid_argument("SSRF blocked: private IPv6 " + host);
            }
            return;
        }

        // It's a hostname -- resolve it and check ALL returned addresses.
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* result = nullptr;
        int ret = getaddrinfo(host.c_str(), nullptr, &hints, &result);
        if (ret != 0) {
            throw std::invalid_argument(
                "SSRF blocked: cannot resolve host " + host);
        }

        // RAII cleanup for addrinfo.
        struct AddrInfoGuard {
            struct addrinfo* p;
            ~AddrInfoGuard() { if (p) freeaddrinfo(p); }
        } guard{result};

        for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
            if (rp->ai_family == AF_INET) {
                auto* sa = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
                if (is_private_ipv4(sa->sin_addr.s_addr)) {
                    throw std::invalid_argument(
                        "SSRF blocked: " + host + " resolves to private IP");
                }
            } else if (rp->ai_family == AF_INET6) {
                auto* sa = reinterpret_cast<struct sockaddr_in6*>(rp->ai_addr);
                if (is_private_ipv6(sa->sin6_addr)) {
                    throw std::invalid_argument(
                        "SSRF blocked: " + host + " resolves to private IPv6");
                }
            }
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
