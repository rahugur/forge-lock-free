#include "workflow/react.h"
#include "utils/tracing.h"

#include <spdlog/spdlog.h>

namespace forge {

WorkflowResult ReActWorkflow::run(Session& session) {
    WorkflowResult result;

    Span root_span(session.id, "react.run");
    root_span.set_attribute("session_id", static_cast<int64_t>(session.id));

    // Build the tools JSON for the LLM request.
    auto tools_json = registry_.to_openai_tools_json();

    spdlog::info("[react] Starting ReAct loop (max {} iterations)",
                 session.config.max_iterations);

    while (!session.is_terminal()) {
        // Guard: cancellation.
        if (session.cancel_requested.load(std::memory_order_acquire)) {
            session.set_error("Cancelled by user");
            session.state.store(SessionState::CANCELLED, std::memory_order_release);
            spdlog::info("[react] Session {} cancelled", session.id);
            break;
        }

        // Guard: max iterations.
        if (session.exceeded_max_iterations()) {
            session.set_error("Maximum iterations (" +
                std::to_string(session.config.max_iterations) + ") exceeded");
            session.state.store(SessionState::FAILED, std::memory_order_release);
            spdlog::warn("[react] {}", session.get_error());
            break;
        }

        // Guard: timeout.
        if (session.is_timed_out()) {
            session.set_error("Session timed out");
            session.state.store(SessionState::FAILED, std::memory_order_release);
            spdlog::warn("[react] {}", session.get_error());
            break;
        }

        session.state.store(SessionState::WAITING_FOR_LLM, std::memory_order_release);
        session.step_count.fetch_add(1, std::memory_order_relaxed);

        spdlog::info("[react] Step {}: calling LLM...", session.step_count.load(std::memory_order_relaxed));

        // 1. Call the LLM.
        auto llm_span = root_span.child("llm.complete");
        auto messages = session.history.to_json();
        auto llm_future = llm_.complete(messages, tools_json);
        auto llm_response = llm_future.get();
        llm_span.set_attribute("prompt_tokens", static_cast<int64_t>(llm_response.prompt_tokens));
        llm_span.set_attribute("completion_tokens", static_cast<int64_t>(llm_response.completion_tokens));
        llm_span.set_status(llm_response.error.empty());

        if (!llm_response.error.empty()) {
            session.set_error("LLM error: " + llm_response.error);
            session.state.store(SessionState::FAILED, std::memory_order_release);
            spdlog::error("[react] {}", session.get_error());
            break;
        }

        result.total_prompt_tokens += llm_response.prompt_tokens;
        result.total_completion_tokens += llm_response.completion_tokens;

        auto& msg = llm_response.message;
        spdlog::info("[react] Step {}: LLM returned (tool_calls={}, content_len={})",
                     session.step_count.load(std::memory_order_relaxed),
                     msg.tool_calls.size(),
                     msg.content.size());

        // 2. Append assistant message to history.
        session.history.push(msg);

        // 3. Check if the LLM wants to call tools.
        if (msg.has_tool_calls()) {
            session.state.store(SessionState::EXECUTING_TOOLS, std::memory_order_release);

            spdlog::info("[react] Executing {} tool call(s)...",
                         msg.tool_calls.size());

            // Execute all tool calls (concurrently via the executor).
            auto tool_span = root_span.child("tools.execute_batch");
            tool_span.set_attribute("tool_count", static_cast<int64_t>(msg.tool_calls.size()));
            auto tool_results = executor_.execute_batch(msg.tool_calls);
            tool_span.set_status(true);

            // Append tool results as "tool" messages.
            for (auto& tr : tool_results) {
                Message tool_msg;
                tool_msg.role = "tool";
                tool_msg.content = tr.output;
                tool_msg.tool_call_id = tr.tool_call_id;
                session.history.push(std::move(tool_msg));

                spdlog::debug("[react] Tool result ({}): {}",
                              tr.tool_call_id,
                              tr.output.substr(0, 100));
            }

            // Loop: go back to WAITING_FOR_LLM.
            continue;
        }

        // 4. No tool calls — this is the final answer.
        if (msg.has_content()) {
            session.set_final_answer(msg.content);
            session.state.store(SessionState::COMPLETED, std::memory_order_release);
            spdlog::info("[react] Final answer received ({} chars)",
                         msg.content.size());
            break;
        }

        // 5. No content, no tool calls — unexpected.
        session.set_error("LLM returned empty response (no content, no tool calls)");
        session.state.store(SessionState::FAILED, std::memory_order_release);
        spdlog::warn("[react] {}", session.error);
        break;
    }

    result.success = (session.state.load(std::memory_order_acquire) == SessionState::COMPLETED);
    result.answer = session.get_final_answer();
    result.error = session.get_error();
    result.steps_taken = session.step_count.load(std::memory_order_relaxed);

    root_span.set_attribute("steps", static_cast<int64_t>(result.steps_taken));
    root_span.set_status(result.success, result.error);

    spdlog::info("[react] {} after {} steps (tokens: {}+{})",
                 result.success ? "Completed" : "Failed",
                 result.steps_taken,
                 result.total_prompt_tokens,
                 result.total_completion_tokens);

    return result;
}

}  // namespace forge
