// Tests for Task DAG executor.
#include <catch2/catch_test_macros.hpp>

#include "workflow/task_dag.h"

#include "core/thread_pool.h"
#include "tools/builtin/calculator.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"

TEST_CASE("TaskDAG: linear chain", "[task_dag]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::TaskDAG dag;

    forge::ToolCall call1;
    call1.id = "c1";
    call1.name = "calculator";
    call1.arguments_json = R"({"expression": "2+2"})";
    auto n0 = dag.add_node(std::move(call1));

    forge::ToolCall call2;
    call2.id = "c2";
    call2.name = "calculator";
    call2.arguments_json = R"({"expression": "3+3"})";
    dag.add_node(std::move(call2), {n0});

    REQUIRE(dag.size() == 2);

    auto results = dag.execute(executor, pool);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].output == "4");
    REQUIRE(results[1].output == "6");
}

TEST_CASE("TaskDAG: parallel independent nodes", "[task_dag]") {
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::TaskDAG dag;

    forge::ToolCall call1;
    call1.id = "c1";
    call1.name = "calculator";
    call1.arguments_json = R"({"expression": "1+1"})";
    dag.add_node(std::move(call1));

    forge::ToolCall call2;
    call2.id = "c2";
    call2.name = "calculator";
    call2.arguments_json = R"({"expression": "2+2"})";
    dag.add_node(std::move(call2));

    forge::ToolCall call3;
    call3.id = "c3";
    call3.name = "calculator";
    call3.arguments_json = R"({"expression": "3+3"})";
    dag.add_node(std::move(call3));

    auto results = dag.execute(executor, pool);
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].output == "2");
    REQUIRE(results[1].output == "4");
    REQUIRE(results[2].output == "6");
}

TEST_CASE("TaskDAG: placeholder substitution", "[task_dag]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::ToolExecutor executor(pool, registry);

    forge::TaskDAG dag;

    // Node 0: compute 2+3 = 5
    forge::ToolCall call1;
    call1.id = "c1";
    call1.name = "calculator";
    call1.arguments_json = R"({"expression": "2+3"})";
    auto n0 = dag.add_node(std::move(call1));

    // Node 1: compute {{node_0_result}} * 2 = 5*2 = 10
    forge::ToolCall call2;
    call2.id = "c2";
    call2.name = "calculator";
    call2.arguments_json = R"({"expression": "{{node_0_result}}*2"})";
    dag.add_node(std::move(call2), {n0});

    auto results = dag.execute(executor, pool);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].output == "5");
    REQUIRE(results[1].output == "10");
}

TEST_CASE("TaskDAG: cycle detection", "[task_dag]") {
    forge::TaskDAG dag;

    forge::ToolCall call1;
    call1.id = "c1";
    call1.name = "calculator";
    call1.arguments_json = R"({"expression": "1+1"})";
    dag.add_node(std::move(call1));

    // Can't create a cycle with add_node since we validate deps < id.
    // But we can test the error for future-node dependency.
    forge::ToolCall call2;
    call2.id = "c2";
    call2.name = "calculator";
    call2.arguments_json = R"({"expression": "2+2"})";

    REQUIRE_THROWS_AS(
        dag.add_node(std::move(call2), {5}),
        std::invalid_argument
    );
}

TEST_CASE("TaskDAG: empty dag", "[task_dag]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    forge::TaskDAG dag;
    REQUIRE(dag.size() == 0);

    auto results = dag.execute(executor, pool);
    REQUIRE(results.empty());
}
