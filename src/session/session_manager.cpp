#include "session/session_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace forge {

SessionManager::SessionManager(ThreadPool& pool, ILLMClient& llm,
                               ToolExecutor& executor,
                               const ToolRegistry& registry,
                               const WorkflowFactory& factory,
                               unsigned max_sessions)
    : pool_(pool), llm_(llm), executor_(executor),
      registry_(registry), factory_(factory), max_sessions_(max_sessions) {
    spdlog::info("SessionManager: capacity={}", max_sessions_);
}

SessionManager::~SessionManager() {
    // Wait for all in-flight workflow tasks to complete.
    std::vector<Future<void>> tasks;
    {
        std::lock_guard lock(inflight_mutex_);
        tasks = std::move(inflight_);
    }
    for (auto& f : tasks) {
        if (f.valid()) {
            f.get();
        }
    }
}

std::variant<uint64_t, std::string>
SessionManager::create_session(CreateSessionRequest req) {
    if (!factory_.has(req.workflow)) {
        return std::string("Unknown workflow: " + req.workflow);
    }

    auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
    auto session = std::make_shared<Session>(id, req.config);
    session->initial_prompt = req.prompt;
    session->state = SessionState::CREATED;

    // Set up conversation history.
    if (!req.system_message.empty()) {
        Message sys;
        sys.role = "system";
        sys.content = req.system_message;
        session->history.push(std::move(sys));
    }

    Message user;
    user.role = "user";
    user.content = req.prompt;
    session->history.push(std::move(user));

    // Atomic capacity check + insert to prevent TOCTOU race.
    if (!sessions_.insert_if_under(id, session, max_sessions_)) {
        return std::string("At capacity (" +
            std::to_string(max_sessions_) + " sessions)");
    }

    spdlog::info("Session {} created (prompt: \"{}...\")",
                 id, req.prompt.substr(0, 40));

    // Dispatch the workflow on the thread pool and track the future.
    auto sess_ptr = session;  // capture a copy of the shared_ptr
    auto workflow_name = req.workflow;
    auto fut = pool_.submit([this, sess_ptr, workflow_name]() {
        sess_ptr->state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);

        auto wf = factory_.create(workflow_name, llm_, executor_, registry_, pool_);
        auto result = wf->run(*sess_ptr);

        spdlog::info("Session {} finished: {} ({} steps, workflow={})",
                     sess_ptr->id,
                     result.success ? "success" : "failed",
                     result.steps_taken,
                     workflow_name);
    });

    {
        std::lock_guard lock(inflight_mutex_);
        // Clean up completed futures before adding a new one.
        inflight_.erase(
            std::remove_if(inflight_.begin(), inflight_.end(),
                [](const Future<void>& f) { return f.ready(); }),
            inflight_.end());
        inflight_.push_back(std::move(fut));
    }

    return id;
}

std::optional<SessionInfo>
SessionManager::get_session(uint64_t id) const {
    SessionInfo info;
    bool found = false;

    sessions_.with_value(id, [&](const std::shared_ptr<Session>& s) {
        info.id = s->id;
        info.state = s->state.load(std::memory_order_acquire);
        info.step_count = s->step_count.load(std::memory_order_relaxed);
        info.final_answer = s->get_final_answer();
        info.error = s->get_error();
        found = true;
    });

    if (!found) return std::nullopt;
    return info;
}

bool SessionManager::cancel_session(uint64_t id) {
    return sessions_.mutate(id, [](std::shared_ptr<Session>& s) {
        s->cancel_requested.store(true, std::memory_order_release);
    });
}

std::vector<uint64_t> SessionManager::list_sessions() const {
    return sessions_.keys();
}

size_t SessionManager::gc(std::chrono::seconds max_age) {
    auto now = std::chrono::steady_clock::now();
    auto keys = sessions_.keys();
    size_t reaped = 0;

    for (auto id : keys) {
        bool should_erase = false;

        sessions_.with_value(id, [&](const std::shared_ptr<Session>& s) {
            if (s->is_terminal()) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - s->created_at);
                if (age >= max_age) {
                    should_erase = true;
                }
            }
        });

        if (should_erase) {
            sessions_.erase(id);
            ++reaped;
        }
    }

    if (reaped > 0) {
        spdlog::debug("GC: reaped {} terminal sessions", reaped);
    }
    return reaped;
}

size_t SessionManager::active_count() const {
    return sessions_.size();
}

}  // namespace forge
