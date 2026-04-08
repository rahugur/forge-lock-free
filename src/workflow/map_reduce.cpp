#include "workflow/map_reduce.h"
#include "workflow/react.h"

#include <spdlog/spdlog.h>

#include <mutex>

namespace forge {

std::vector<MapReduceWorkflow::SubTask>
MapReduceWorkflow::decompose(Session& session, WorkflowResult& result) {
    spdlog::info("[map-reduce] Decomposing task...");

    Message sys;
    sys.role = "system";
    sys.content =
        "You are a task decomposition assistant. Break down the user's request "
        "into independent sub-tasks that can be executed in parallel. "
        "Respond with ONLY a JSON object in this format:\n"
        R"({"tasks": [{"id": 1, "description": "sub-task description"}, ...]})"
        "\nDo not include any other text.";

    Json messages = Json::array();
    messages.push_back(sys.to_json());

    for (auto& m : session.history.messages()) {
        if (m.role == "user") {
            messages.push_back(m.to_json());
            break;
        }
    }

    auto llm_future = llm_.complete(messages);
    auto llm_response = llm_future.get();

    result.total_prompt_tokens += llm_response.prompt_tokens;
    result.total_completion_tokens += llm_response.completion_tokens;

    if (!llm_response.error.empty()) {
        spdlog::error("[map-reduce] Decomposition failed: {}", llm_response.error);
        return {};
    }

    std::vector<SubTask> tasks;
    try {
        auto j = Json::parse(llm_response.message.content);
        if (j.contains("tasks") && j["tasks"].is_array()) {
            for (auto& t : j["tasks"]) {
                SubTask task;
                task.id = t.value("id", static_cast<int>(tasks.size() + 1));
                task.description = t.value("description", "");
                tasks.push_back(std::move(task));
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("[map-reduce] Failed to parse decomposition: {}. "
                     "Treating entire response as single task.", e.what());
        SubTask task;
        task.id = 1;
        task.description = llm_response.message.content;
        tasks.push_back(std::move(task));
    }

    spdlog::info("[map-reduce] Decomposed into {} sub-tasks", tasks.size());
    return tasks;
}

std::string MapReduceWorkflow::reduce(
    Session& session, const std::vector<SubTask>& tasks, WorkflowResult& result) {
    spdlog::info("[map-reduce] Reducing {} sub-task results...", tasks.size());

    Json messages = Json::array();

    Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant. Aggregate the results of the "
                  "following sub-tasks into a final, coherent answer.";
    messages.push_back(sys.to_json());

    for (auto& m : session.history.messages()) {
        if (m.role == "user") {
            messages.push_back(m.to_json());
            break;
        }
    }

    std::string summary;
    for (auto& t : tasks) {
        summary += "Sub-task " + std::to_string(t.id) + ": " + t.description +
                   "\nResult: " + t.result +
                   "\nSuccess: " + (t.success ? "yes" : "no") + "\n\n";
    }

    Message results_msg;
    results_msg.role = "user";
    results_msg.content = "Here are the sub-task results:\n\n" + summary +
                          "\nPlease aggregate these into a final answer.";
    messages.push_back(results_msg.to_json());

    auto llm_future = llm_.complete(messages);
    auto llm_response = llm_future.get();

    result.total_prompt_tokens += llm_response.prompt_tokens;
    result.total_completion_tokens += llm_response.completion_tokens;

    if (!llm_response.error.empty()) {
        spdlog::error("[map-reduce] Reduce failed: {}", llm_response.error);
        return "";
    }

    return llm_response.message.content;
}

WorkflowResult MapReduceWorkflow::run(Session& session) {
    WorkflowResult result;

    // Phase 1: Decompose.
    session.state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);
    auto tasks = decompose(session, result);

    if (tasks.empty()) {
        session.set_error("Failed to decompose task");
        session.state.store(SessionState::FAILED, std::memory_order_release);
        result.error = session.get_error();
        return result;
    }

    // Phase 2: Execute sub-tasks in parallel via thread pool.
    session.state.store(SessionState::EXECUTING_TOOLS, std::memory_order_release);

    std::mutex mu;
    std::vector<Future<bool>> futures;
    futures.reserve(tasks.size());

    for (size_t i = 0; i < tasks.size(); ++i) {
        auto future = pool_.submit([this, &tasks, &result, &mu, &session, i]() {
            // Check cancellation.
            if (session.cancel_requested.load(std::memory_order_acquire)) {
                return false;
            }

            auto& task = tasks[i];

            spdlog::info("[map-reduce] Executing sub-task {}: {}",
                         task.id, task.description.substr(0, 60));

            session.step_count.fetch_add(1, std::memory_order_relaxed);

            Session sub_session(0, session.config);
            Message sys;
            sys.role = "system";
            sys.content = "You are a helpful assistant with access to tools. "
                          "Complete the following task.";
            sub_session.history.push(std::move(sys));

            Message user;
            user.role = "user";
            user.content = task.description;
            sub_session.history.push(std::move(user));

            ReActWorkflow react(llm_, executor_, registry_);
            auto sub_result = react.run(sub_session);

            {
                std::lock_guard lock(mu);
                result.total_prompt_tokens += sub_result.total_prompt_tokens;
                result.total_completion_tokens += sub_result.total_completion_tokens;
            }

            task.success = sub_result.success;
            task.result = sub_result.success ? sub_result.answer : sub_result.error;

            return sub_result.success;
        });
        futures.push_back(std::move(future));
    }

    // Wait for all sub-tasks.
    for (auto& f : futures) {
        f.get();
    }

    // Check cancellation.
    if (session.cancel_requested.load(std::memory_order_acquire)) {
        session.set_error("Cancelled by user");
        session.state.store(SessionState::CANCELLED, std::memory_order_release);
        result.error = session.get_error();
        return result;
    }

    // Phase 3: Reduce.
    session.state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);
    auto answer = reduce(session, tasks, result);

    if (answer.empty()) {
        session.set_error("Reduce phase failed");
        session.state.store(SessionState::FAILED, std::memory_order_release);
        result.error = session.get_error();
        return result;
    }

    session.set_final_answer(answer);
    session.state.store(SessionState::COMPLETED, std::memory_order_release);
    result.success = true;
    result.answer = answer;
    result.steps_taken = session.step_count.load(std::memory_order_relaxed);

    spdlog::info("[map-reduce] Completed ({} sub-tasks, tokens: {}+{})",
                 tasks.size(),
                 result.total_prompt_tokens,
                 result.total_completion_tokens);

    return result;
}

}  // namespace forge
