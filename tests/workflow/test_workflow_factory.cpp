// Tests for WorkflowFactory.
#include <catch2/catch_test_macros.hpp>

#include "workflow/react.h"
#include "workflow/workflow_factory.h"

#include "../common/scripted_mock_llm.h"
#include "core/thread_pool.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"

TEST_CASE("WorkflowFactory: register and create", "[workflow_factory]") {
    forge::WorkflowFactory factory;

    factory.register_workflow("react", [](forge::ILLMClient& llm,
                                          forge::ToolExecutor& exec,
                                          const forge::ToolRegistry& reg,
                                          forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(llm, exec, reg);
    });

    REQUIRE(factory.has("react"));
    REQUIRE_FALSE(factory.has("nonexistent"));

    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    auto wf = factory.create("react", llm, executor, registry, pool);
    REQUIRE(wf != nullptr);
    REQUIRE(wf->name() == "react");
}

TEST_CASE("WorkflowFactory: unknown workflow throws", "[workflow_factory]") {
    forge::WorkflowFactory factory;

    forge::testing::ScriptedMockLLM llm;
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    REQUIRE_THROWS_AS(
        factory.create("nonexistent", llm, executor, registry, pool),
        std::invalid_argument
    );
}

TEST_CASE("WorkflowFactory: names", "[workflow_factory]") {
    forge::WorkflowFactory factory;

    factory.register_workflow("react", [](forge::ILLMClient& llm,
                                          forge::ToolExecutor& exec,
                                          const forge::ToolRegistry& reg,
                                          forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(llm, exec, reg);
    });

    auto names = factory.names();
    REQUIRE(names.size() == 1);
    REQUIRE(names[0] == "react");
}
