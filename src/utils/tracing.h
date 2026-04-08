#pragma once
// Lightweight structured tracing.
// RAII Span logs a structured JSON line on destruction.
// OpenTelemetry-compatible export deferred to Phase 4.

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace forge {

namespace detail {
inline std::atomic<uint64_t>& span_id_counter() {
    static std::atomic<uint64_t> counter{1};
    return counter;
}
}  // namespace detail

struct SpanContext {
    uint64_t trace_id = 0;
    uint64_t span_id = 0;
    uint64_t parent_span_id = 0;
};

class Span {
public:
    Span(uint64_t trace_id, std::string operation)
        : operation_(std::move(operation)),
          start_(std::chrono::steady_clock::now()) {
        ctx_.trace_id = trace_id;
        ctx_.span_id = detail::span_id_counter().fetch_add(1,
            std::memory_order_relaxed);
        ctx_.parent_span_id = 0;
    }

    ~Span() {
        auto end = std::chrono::steady_clock::now();
        auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_).count();

        // Build attribute string.
        std::string attrs = "{";
        bool first = true;
        for (auto& [k, v] : attributes_) {
            if (!first) attrs += ",";
            attrs += "\"" + k + "\":\"" + v + "\"";
            first = false;
        }
        attrs += "}";

        spdlog::info(
            "{{\"trace_id\":{},\"span_id\":{},\"parent\":{},\"op\":\"{}\","
            "\"dur_us\":{},\"ok\":{},\"status\":\"{}\",\"attrs\":{}}}",
            ctx_.trace_id, ctx_.span_id, ctx_.parent_span_id,
            operation_, dur_us, ok_ ? "true" : "false",
            status_message_, attrs);
    }

    // Non-copyable, movable only to support return from child().
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&& other) noexcept
        : ctx_(other.ctx_), operation_(std::move(other.operation_)),
          start_(other.start_), attributes_(std::move(other.attributes_)),
          ok_(other.ok_), status_message_(std::move(other.status_message_)),
          moved_(false) {
        other.moved_ = true;
    }

    void set_attribute(const std::string& key, const std::string& value) {
        attributes_[key] = value;
    }

    void set_attribute(const std::string& key, int64_t value) {
        attributes_[key] = std::to_string(value);
    }

    void set_status(bool ok, const std::string& message = "") {
        ok_ = ok;
        status_message_ = message;
    }

    SpanContext context() const { return ctx_; }

    /// Create a child span.
    Span child(const std::string& operation) {
        Span s(ctx_.trace_id, operation);
        s.ctx_.parent_span_id = ctx_.span_id;
        return s;
    }

private:
    SpanContext ctx_;
    std::string operation_;
    std::chrono::steady_clock::time_point start_;
    std::unordered_map<std::string, std::string> attributes_;
    bool ok_ = true;
    std::string status_message_;
    bool moved_ = false;
};

}  // namespace forge
