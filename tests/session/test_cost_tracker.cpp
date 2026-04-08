// Tests for CostTracker.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "session/cost_tracker.h"

using Catch::Approx;

TEST_CASE("CostTracker: basic recording", "[cost_tracker]") {
    forge::CostTracker tracker;
    tracker.set_pricing("gpt-4o", {0.005, 0.015});

    tracker.record("gpt-4o", 100, 50);

    REQUIRE(tracker.total_prompt_tokens() == 100);
    REQUIRE(tracker.total_completion_tokens() == 50);

    double expected = (100 / 1000.0) * 0.005 + (50 / 1000.0) * 0.015;
    REQUIRE(tracker.total_cost() == Approx(expected));
}

TEST_CASE("CostTracker: multiple models", "[cost_tracker]") {
    forge::CostTracker tracker;
    tracker.set_pricing("gpt-4o", {0.005, 0.015});
    tracker.set_pricing("gpt-3.5", {0.0005, 0.0015});

    tracker.record("gpt-4o", 1000, 500);
    tracker.record("gpt-3.5", 2000, 1000);

    REQUIRE(tracker.total_prompt_tokens() == 3000);
    REQUIRE(tracker.total_completion_tokens() == 1500);

    double cost_4o = (1000 / 1000.0) * 0.005 + (500 / 1000.0) * 0.015;
    double cost_35 = (2000 / 1000.0) * 0.0005 + (1000 / 1000.0) * 0.0015;
    REQUIRE(tracker.total_cost() == Approx(cost_4o + cost_35));
}

TEST_CASE("CostTracker: unknown model has zero cost", "[cost_tracker]") {
    forge::CostTracker tracker;

    tracker.record("unknown-model", 1000, 500);

    REQUIRE(tracker.total_prompt_tokens() == 1000);
    REQUIRE(tracker.total_completion_tokens() == 500);
    REQUIRE(tracker.total_cost() == 0.0);
}

TEST_CASE("CostTracker: to_json", "[cost_tracker]") {
    forge::CostTracker tracker;
    tracker.set_pricing("gpt-4o", {0.005, 0.015});
    tracker.record("gpt-4o", 100, 50);

    auto j = tracker.to_json();
    REQUIRE(j["prompt_tokens"] == 100);
    REQUIRE(j["completion_tokens"] == 50);
    REQUIRE(j["total_cost"].get<double>() > 0.0);
}
