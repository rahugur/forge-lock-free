// Tests for global Metrics singleton.
#include <catch2/catch_test_macros.hpp>

#include "utils/metrics.h"

TEST_CASE("Metrics: basic counters", "[metrics]") {
    auto& m = forge::Metrics::instance();
    m.reset();

    m.inc_sessions_created();
    m.inc_sessions_created();
    m.inc_sessions_completed();
    m.inc_sessions_failed();
    m.add_tokens(100, 50);
    m.inc_tool_calls(3);

    REQUIRE(m.sessions_created() == 2);
    REQUIRE(m.sessions_completed() == 1);
    REQUIRE(m.sessions_failed() == 1);
    REQUIRE(m.total_prompt_tokens() == 100);
    REQUIRE(m.total_completion_tokens() == 50);
    REQUIRE(m.total_tool_calls() == 3);
}

TEST_CASE("Metrics: reset", "[metrics]") {
    auto& m = forge::Metrics::instance();
    m.inc_sessions_created();
    m.add_tokens(50, 25);

    m.reset();

    REQUIRE(m.sessions_created() == 0);
    REQUIRE(m.sessions_completed() == 0);
    REQUIRE(m.sessions_failed() == 0);
    REQUIRE(m.total_prompt_tokens() == 0);
    REQUIRE(m.total_completion_tokens() == 0);
    REQUIRE(m.total_tool_calls() == 0);
}

TEST_CASE("Metrics: to_json", "[metrics]") {
    auto& m = forge::Metrics::instance();
    m.reset();

    m.inc_sessions_created();
    m.inc_sessions_completed();
    m.add_tokens(100, 50);
    m.inc_tool_calls();

    auto j = m.to_json();
    REQUIRE(j["sessions_created"] == 1);
    REQUIRE(j["sessions_completed"] == 1);
    REQUIRE(j["sessions_failed"] == 0);
    REQUIRE(j["total_prompt_tokens"] == 100);
    REQUIRE(j["total_completion_tokens"] == 50);
    REQUIRE(j["total_tool_calls"] == 1);
}
