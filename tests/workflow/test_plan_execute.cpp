// Tests for Plan-and-Execute workflow.
#include <catch2/catch_test_macros.hpp>

#include "workflow/plan_execute.h"

#include "../common/scripted_mock_llm.h"
#include "core/thread_pool.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"

TEST_CASE("PlanExecuteWorkflow: basic 2-step plan", "[plan_execute]") {
    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    // Response 1: plan generation.
    llm.add_text_response(R"({"steps": [
        {"id": 1, "description": "Compute 2+2"},
        {"id": 2, "description": "Compute 3+3"}
    ]})");

    // Step 1 ReAct: tool call then answer.
    llm.add_tool_call_response("calculator", R"({"expression": "2+2"})");
    llm.add_text_response("4");

    // Step 2 ReAct: tool call then answer.
    llm.add_tool_call_response("calculator", R"({"expression": "3+3"})");
    llm.add_text_response("6");

    // Synthesis response.
    llm.add_text_response("2+2=4 and 3+3=6");

    forge::Session session(1);
    forge::Message user;
    user.role = "user";
    user.content = "Compute 2+2 and 3+3";
    session.history.push(std::move(user));

    forge::PlanExecuteWorkflow pe(llm, executor, registry, pool);
    auto result = pe.run(session);

    REQUIRE(result.success);
    REQUIRE(result.answer == "2+2=4 and 3+3=6");
    REQUIRE(result.total_prompt_tokens > 0);
    REQUIRE(result.total_completion_tokens > 0);
    REQUIRE(pe.name() == "plan-execute");
}

TEST_CASE("PlanExecuteWorkflow: plan generation failure", "[plan_execute]") {
    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    // LLM returns an error for plan generation.
    llm.add_error_response("API error");

    forge::Session session(1);
    forge::Message user;
    user.role = "user";
    user.content = "test";
    session.history.push(std::move(user));

    forge::PlanExecuteWorkflow pe(llm, executor, registry, pool);
    auto result = pe.run(session);

    REQUIRE_FALSE(result.success);
    REQUIRE(session.state.load() == forge::SessionState::FAILED);
}
