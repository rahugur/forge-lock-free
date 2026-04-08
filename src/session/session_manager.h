#pragma once
// Concurrent session manager.
// Creates, tracks, and garbage-collects agent sessions.
// Backed by ConcurrentMap for lock-free reads.

#include "core/concurrent_map.h"
#include "core/future.h"
#include "core/thread_pool.h"
#include "llm/llm_client_interface.h"
#include "session/session.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/workflow_factory.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace forge {

struct CreateSessionRequest {
    std::string prompt;
    std::string system_message = "You are a helpful assistant.";
    std::string workflow = "react";
    std::vector<std::string> tools;  // empty = all registered tools
    SessionConfig config;
};

struct SessionInfo {
    uint64_t id = 0;
    SessionState state = SessionState::CREATED;
    uint32_t step_count = 0;
    std::string final_answer;
    std::string error;
};

class SessionManager {
public:
    SessionManager(ThreadPool& pool, ILLMClient& llm,
                   ToolExecutor& executor, const ToolRegistry& registry,
                   const WorkflowFactory& factory, unsigned max_sessions);
    ~SessionManager();

    // Non-copyable, non-movable.
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /// Create a new session and dispatch it on the thread pool.
    /// Returns session ID on success, or error string on failure.
    std::variant<uint64_t, std::string> create_session(CreateSessionRequest req);

    /// Query session state.  Returns nullopt if not found.
    std::optional<SessionInfo> get_session(uint64_t id) const;

    /// Request cooperative cancellation.  Returns false if not found.
    bool cancel_session(uint64_t id);

    /// List all session IDs (approximate snapshot).
    std::vector<uint64_t> list_sessions() const;

    /// Reap terminal sessions older than max_age.
    /// Returns number of sessions reaped.
    size_t gc(std::chrono::seconds max_age);

    /// Current session count.
    size_t active_count() const;

private:
    ThreadPool& pool_;
    ILLMClient& llm_;
    ToolExecutor& executor_;
    const ToolRegistry& registry_;
    const WorkflowFactory& factory_;
    unsigned max_sessions_;

    std::atomic<uint64_t> next_id_{1};
    ConcurrentMap<uint64_t, std::shared_ptr<Session>> sessions_;

    // Track in-flight workflow futures so destructor can wait for them.
    std::mutex inflight_mutex_;
    std::vector<Future<void>> inflight_;
};

}  // namespace forge
