#pragma once
// Global runtime metrics.
// All counters are atomic for lock-free concurrent updates.

#include "utils/json.h"

#include <atomic>
#include <cstdint>

namespace forge {

class Metrics {
public:
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    void inc_sessions_created() { sessions_created_.fetch_add(1, std::memory_order_relaxed); }
    void inc_sessions_completed() { sessions_completed_.fetch_add(1, std::memory_order_relaxed); }
    void inc_sessions_failed() { sessions_failed_.fetch_add(1, std::memory_order_relaxed); }
    void add_tokens(int prompt, int completion) {
        prompt_tokens_.fetch_add(prompt, std::memory_order_relaxed);
        completion_tokens_.fetch_add(completion, std::memory_order_relaxed);
    }
    void inc_tool_calls(int count = 1) { tool_calls_.fetch_add(count, std::memory_order_relaxed); }

    uint64_t sessions_created() const { return sessions_created_.load(std::memory_order_relaxed); }
    uint64_t sessions_completed() const { return sessions_completed_.load(std::memory_order_relaxed); }
    uint64_t sessions_failed() const { return sessions_failed_.load(std::memory_order_relaxed); }
    uint64_t total_prompt_tokens() const { return prompt_tokens_.load(std::memory_order_relaxed); }
    uint64_t total_completion_tokens() const { return completion_tokens_.load(std::memory_order_relaxed); }
    uint64_t total_tool_calls() const { return tool_calls_.load(std::memory_order_relaxed); }

    Json to_json() const {
        Json j;
        j["sessions_created"] = sessions_created();
        j["sessions_completed"] = sessions_completed();
        j["sessions_failed"] = sessions_failed();
        j["total_prompt_tokens"] = total_prompt_tokens();
        j["total_completion_tokens"] = total_completion_tokens();
        j["total_tool_calls"] = total_tool_calls();
        return j;
    }

    void reset() {
        sessions_created_.store(0, std::memory_order_relaxed);
        sessions_completed_.store(0, std::memory_order_relaxed);
        sessions_failed_.store(0, std::memory_order_relaxed);
        prompt_tokens_.store(0, std::memory_order_relaxed);
        completion_tokens_.store(0, std::memory_order_relaxed);
        tool_calls_.store(0, std::memory_order_relaxed);
    }

private:
    Metrics() = default;

    std::atomic<uint64_t> sessions_created_{0};
    std::atomic<uint64_t> sessions_completed_{0};
    std::atomic<uint64_t> sessions_failed_{0};
    std::atomic<uint64_t> prompt_tokens_{0};
    std::atomic<uint64_t> completion_tokens_{0};
    std::atomic<uint64_t> tool_calls_{0};
};

}  // namespace forge
