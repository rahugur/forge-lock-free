#include "workflow/plan_execute.h"
#include "workflow/react.h"

#include <spdlog/spdlog.h>

namespace forge {

std::vector<PlanExecuteWorkflow::Step>
PlanExecuteWorkflow::generate_plan(Session& session, WorkflowResult& result) {
    spdlog::info("[plan-execute] Generating plan...");

    // Ask the LLM to generate a plan as JSON.
    Message plan_prompt;
    plan_prompt.role = "system";
    plan_prompt.content =
        "You are a planning assistant. Break down the user's request into "
        "a sequence of steps. Respond with ONLY a JSON object in this format:\n"
        R"({"steps": [{"id": 1, "description": "step description"}, ...]})"
        "\nDo not include any other text.";

    Json messages = Json::array();
    messages.push_back(plan_prompt.to_json());

    // Include the user's original prompt.
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
        spdlog::error("[plan-execute] Plan generation failed: {}", llm_response.error);
        return {};
    }

    // Parse the plan JSON.
    std::vector<Step> steps;
    try {
        auto plan_json = Json::parse(llm_response.message.content);
        if (plan_json.contains("steps") && plan_json["steps"].is_array()) {
            for (auto& s : plan_json["steps"]) {
                Step step;
                step.id = s.value("id", static_cast<int>(steps.size() + 1));
                step.description = s.value("description", "");
                steps.push_back(std::move(step));
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("[plan-execute] Failed to parse plan JSON: {}. "
                     "Treating entire response as single step.", e.what());
        Step step;
        step.id = 1;
        step.description = llm_response.message.content;
        steps.push_back(std::move(step));
    }

    spdlog::info("[plan-execute] Plan has {} steps", steps.size());
    return steps;
}

std::string PlanExecuteWorkflow::synthesize(
    Session& session, const std::vector<Step>& steps, WorkflowResult& result) {
    spdlog::info("[plan-execute] Synthesizing final answer...");

    Json messages = Json::array();

    // System message.
    Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant. Synthesize the results of the "
                  "following steps into a final, coherent answer.";
    messages.push_back(sys.to_json());

    // Include original user prompt.
    for (auto& m : session.history.messages()) {
        if (m.role == "user") {
            messages.push_back(m.to_json());
            break;
        }
    }

    // Include step results.
    std::string step_summary;
    for (auto& s : steps) {
        step_summary += "Step " + std::to_string(s.id) + ": " + s.description +
                        "\nResult: " + s.result + "\n\n";
    }

    Message step_msg;
    step_msg.role = "user";
    step_msg.content = "Here are the results of each step:\n\n" + step_summary +
                       "\nPlease synthesize these into a final answer.";
    messages.push_back(step_msg.to_json());

    auto llm_future = llm_.complete(messages);
    auto llm_response = llm_future.get();

    result.total_prompt_tokens += llm_response.prompt_tokens;
    result.total_completion_tokens += llm_response.completion_tokens;

    if (!llm_response.error.empty()) {
        spdlog::error("[plan-execute] Synthesis failed: {}", llm_response.error);
        return "";
    }

    return llm_response.message.content;
}

WorkflowResult PlanExecuteWorkflow::run(Session& session) {
    WorkflowResult result;

    // Phase 1: Generate plan.
    session.state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);
    auto steps = generate_plan(session, result);

    if (steps.empty()) {
        session.set_error("Failed to generate plan");
        session.state.store(SessionState::FAILED, std::memory_order_release);
        result.error = session.get_error();
        return result;
    }

    // Phase 2: Execute each step via ReAct sub-sessions.
    session.state.store(SessionState::EXECUTING_TOOLS, std::memory_order_release);

    for (auto& step : steps) {
        // Check cancellation between steps.
        if (session.cancel_requested.load(std::memory_order_acquire)) {
            session.set_error("Cancelled by user");
            session.state.store(SessionState::CANCELLED, std::memory_order_release);
            result.error = session.get_error();
            return result;
        }

        spdlog::info("[plan-execute] Executing step {}: {}", step.id,
                     step.description.substr(0, 60));

        session.step_count.fetch_add(1, std::memory_order_relaxed);

        // Create a local sub-session for this step.
        Session sub_session(0, session.config);
        Message sys;
        sys.role = "system";
        sys.content = "You are a helpful assistant with access to tools. "
                      "Complete the following task.";
        sub_session.history.push(std::move(sys));

        Message user;
        user.role = "user";
        user.content = step.description;
        sub_session.history.push(std::move(user));

        ReActWorkflow react(llm_, executor_, registry_);
        auto step_result = react.run(sub_session);

        result.total_prompt_tokens += step_result.total_prompt_tokens;
        result.total_completion_tokens += step_result.total_completion_tokens;

        if (step_result.success) {
            step.result = step_result.answer;
        } else {
            step.result = "Failed: " + step_result.error;
            spdlog::warn("[plan-execute] Step {} failed: {}", step.id, step_result.error);
        }
    }

    // Phase 3: Synthesize.
    session.state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);
    auto answer = synthesize(session, steps, result);

    if (answer.empty()) {
        session.set_error("Synthesis failed");
        session.state.store(SessionState::FAILED, std::memory_order_release);
        result.error = session.get_error();
        return result;
    }

    session.set_final_answer(answer);
    session.state.store(SessionState::COMPLETED, std::memory_order_release);
    result.success = true;
    result.answer = answer;
    result.steps_taken = session.step_count.load(std::memory_order_relaxed);

    spdlog::info("[plan-execute] Completed after {} steps (tokens: {}+{})",
                 result.steps_taken,
                 result.total_prompt_tokens,
                 result.total_completion_tokens);

    return result;
}

}  // namespace forge
