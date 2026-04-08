// Tests for Map-Reduce workflow.
#include <catch2/catch_test_macros.hpp>

#include "workflow/map_reduce.h"

#include "../common/scripted_mock_llm.h"
#include "core/thread_pool.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"

TEST_CASE("MapReduceWorkflow: basic decomposition", "[map_reduce]") {
    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    // Use handler since parallel execution makes queue order non-deterministic.
    std::atomic<int> call_count{0};
    llm.set_handler([&](const forge::Json& messages, const forge::Json&) -> forge::LLMResponse {
        int n = call_count.fetch_add(1);
        forge::LLMResponse resp;
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;
        resp.message.role = "assistant";

        if (n == 0) {
            // Decomposition: return 2 tasks.
            resp.message.content = R"({"tasks": [
                {"id": 1, "description": "Compute 2+2"},
                {"id": 2, "description": "Compute 3+3"}
            ]})";
        } else {
            // Sub-task or reduce responses — return a simple answer.
            // Check message content to give appropriate response.
            auto msg_str = messages.dump();
            if (msg_str.find("Aggregate") != std::string::npos ||
                msg_str.find("aggregate") != std::string::npos) {
                resp.message.content = "Results: 4 and 6";
            } else {
                resp.message.content = "Done";
            }
        }
        return resp;
    });

    forge::Session session(1);
    forge::Message user;
    user.role = "user";
    user.content = "Compute 2+2 and 3+3 separately";
    session.history.push(std::move(user));

    forge::MapReduceWorkflow mr(llm, executor, registry, pool);
    auto result = mr.run(session);

    REQUIRE(result.success);
    REQUIRE_FALSE(result.answer.empty());
    REQUIRE(result.total_prompt_tokens > 0);
    REQUIRE(mr.name() == "map-reduce");
}

TEST_CASE("MapReduceWorkflow: decomposition failure", "[map_reduce]") {
    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    llm.add_error_response("API error");

    forge::Session session(1);
    forge::Message user;
    user.role = "user";
    user.content = "test";
    session.history.push(std::move(user));

    forge::MapReduceWorkflow mr(llm, executor, registry, pool);
    auto result = mr.run(session);

    REQUIRE_FALSE(result.success);
    REQUIRE(session.state.load() == forge::SessionState::FAILED);
}
