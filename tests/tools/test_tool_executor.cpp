// Tests for the tool executor.
#include <catch2/catch_test_macros.hpp>

#include "core/thread_pool.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "tools/tool_spec.h"
#include "llm/message.h"

#include <chrono>
#include <thread>

TEST_CASE("ToolExecutor: executes a simple tool", "[executor]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;

    forge::ToolSpec spec;
    spec.name = "echo";
    spec.description = "Echoes input";
    spec.timeout = std::chrono::milliseconds(5000);

    registry.register_tool(std::move(spec), [](const std::string& args) -> std::string {
        return "echo: " + args;
    });

    forge::ToolExecutor executor(pool, registry);

    forge::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    call.arguments_json = R"({"text":"hello"})";

    auto result = executor.execute(call);
    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.tool_call_id == "call_1");
    REQUIRE(result.output == R"(echo: {"text":"hello"})");
}

TEST_CASE("ToolExecutor: unknown tool returns error", "[executor]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;
    forge::ToolExecutor executor(pool, registry);

    forge::ToolCall call;
    call.id = "call_2";
    call.name = "nonexistent";

    auto result = executor.execute(call);
    REQUIRE(result.is_error);
    REQUIRE(result.output.find("Unknown tool") != std::string::npos);
}

TEST_CASE("ToolExecutor: timeout on slow tool", "[executor]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;

    forge::ToolSpec spec;
    spec.name = "slow";
    spec.description = "Takes forever";
    spec.timeout = std::chrono::milliseconds(100);

    registry.register_tool(std::move(spec), [](const std::string&) -> std::string {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return "done";
    });

    forge::ToolExecutor executor(pool, registry);

    forge::ToolCall call;
    call.id = "call_3";
    call.name = "slow";

    auto result = executor.execute(call);
    REQUIRE(result.is_error);
    REQUIRE(result.output.find("timed out") != std::string::npos);
}

TEST_CASE("ToolExecutor: tool exception becomes error result", "[executor]") {
    forge::ThreadPool pool(2);
    forge::ToolRegistry registry;

    forge::ToolSpec spec;
    spec.name = "faulty";
    spec.description = "Always throws";
    spec.timeout = std::chrono::milliseconds(5000);

    registry.register_tool(std::move(spec), [](const std::string&) -> std::string {
        throw std::runtime_error("tool broke");
    });

    forge::ToolExecutor executor(pool, registry);

    forge::ToolCall call;
    call.id = "call_4";
    call.name = "faulty";

    auto result = executor.execute(call);
    REQUIRE(result.is_error);
    REQUIRE(result.output.find("tool broke") != std::string::npos);
}

TEST_CASE("ToolExecutor: batch execution", "[executor]") {
    forge::ThreadPool pool(4);
    forge::ToolRegistry registry;

    forge::ToolSpec spec;
    spec.name = "greet";
    spec.description = "Greets";
    spec.timeout = std::chrono::milliseconds(5000);

    registry.register_tool(std::move(spec), [](const std::string& args) -> std::string {
        return "hi " + args;
    });

    forge::ToolExecutor executor(pool, registry);

    std::vector<forge::ToolCall> calls;
    for (int i = 0; i < 5; ++i) {
        forge::ToolCall call;
        call.id = "call_" + std::to_string(i);
        call.name = "greet";
        call.arguments_json = std::to_string(i);
        calls.push_back(std::move(call));
    }

    auto results = executor.execute_batch(calls);
    REQUIRE(results.size() == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE_FALSE(results[i].is_error);
        REQUIRE(results[i].output == "hi " + std::to_string(i));
    }
}
