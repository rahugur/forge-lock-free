#pragma once
// Session: a single agent execution context.
// Contains the conversation history, state, and execution limits.

#include "session/conversation.h"
#include "llm/message.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace forge {

enum class SessionState {
    CREATED,            // session created, workflow not yet started
    WAITING_FOR_LLM,
    EXECUTING_TOOLS,
    WAITING_FOR_HUMAN,
    COMPLETED,
    FAILED,
    CANCELLED           // cancelled by user
};

inline const char* session_state_name(SessionState s) {
    switch (s) {
        case SessionState::CREATED:          return "created";
        case SessionState::WAITING_FOR_LLM:  return "waiting_for_llm";
        case SessionState::EXECUTING_TOOLS:  return "executing_tools";
        case SessionState::WAITING_FOR_HUMAN:return "waiting_for_human";
        case SessionState::COMPLETED:        return "completed";
        case SessionState::FAILED:           return "failed";
        case SessionState::CANCELLED:        return "cancelled";
    }
    return "unknown";
}

struct SessionConfig {
    unsigned max_iterations = 15;
    unsigned max_history = 100;
    std::chrono::seconds timeout{300};
};

struct Session {
    uint64_t id = 0;
    std::atomic<SessionState> state{SessionState::CREATED};
    Conversation history;
    SessionConfig config;
    std::atomic<uint32_t> step_count{0};
    std::chrono::steady_clock::time_point deadline;
    std::chrono::steady_clock::time_point created_at;
    std::string initial_prompt;

    /// Mutex protecting final_answer and error from concurrent access.
    mutable std::mutex result_mutex;
    std::string final_answer;  // guarded by result_mutex
    std::string error;         // guarded by result_mutex

    /// Cooperative cancellation flag.  Set by API, checked by workflow.
    std::atomic<bool> cancel_requested{false};

    void set_final_answer(const std::string& answer) {
        std::lock_guard lock(result_mutex);
        final_answer = answer;
    }

    void set_error(const std::string& err) {
        std::lock_guard lock(result_mutex);
        error = err;
    }

    std::string get_final_answer() const {
        std::lock_guard lock(result_mutex);
        return final_answer;
    }

    std::string get_error() const {
        std::lock_guard lock(result_mutex);
        return error;
    }

    explicit Session(uint64_t session_id, SessionConfig cfg = {})
        : id(session_id),
          history(cfg.max_history),
          config(cfg),
          deadline(std::chrono::steady_clock::now() + cfg.timeout),
          created_at(std::chrono::steady_clock::now()) {}

    bool is_terminal() const {
        auto s = state.load(std::memory_order_acquire);
        return s == SessionState::COMPLETED ||
               s == SessionState::FAILED ||
               s == SessionState::CANCELLED;
    }

    bool is_timed_out() const {
        return std::chrono::steady_clock::now() > deadline;
    }

    bool exceeded_max_iterations() const {
        return step_count >= config.max_iterations;
    }
};

}  // namespace forge
