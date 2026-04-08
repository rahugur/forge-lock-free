// Tests for the calculator tool's expression parser.
#include <catch2/catch_test_macros.hpp>

#include "tools/tool_registry.h"
#include "tools/builtin/calculator.h"

#include <cmath>
#include <string>

// Helper: register calculator then invoke it.
static std::string calc(const std::string& expr) {
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    auto& tool = registry.get("calculator");
    std::string args = R"({"expression":")" + expr + R"("})";
    return tool.handler(args);
}

TEST_CASE("Calculator: basic arithmetic", "[calc]") {
    REQUIRE(calc("2 + 3") == "5");
    REQUIRE(calc("10 - 4") == "6");
    REQUIRE(calc("3 * 7") == "21");
    REQUIRE(calc("15 / 3") == "5");
}

TEST_CASE("Calculator: operator precedence", "[calc]") {
    REQUIRE(calc("2 + 3 * 4") == "14");
    REQUIRE(calc("10 - 2 * 3") == "4");
    REQUIRE(calc("(2 + 3) * 4") == "20");
}

TEST_CASE("Calculator: exponentiation", "[calc]") {
    REQUIRE(calc("2^10") == "1024");
    REQUIRE(calc("3^3") == "27");
    REQUIRE(calc("2^0") == "1");
}

TEST_CASE("Calculator: nested parentheses", "[calc]") {
    REQUIRE(calc("((2 + 3) * (4 - 1))") == "15");
    REQUIRE(calc("(2^(1+2))") == "8");
}

TEST_CASE("Calculator: unary minus", "[calc]") {
    REQUIRE(calc("-5") == "-5");
    REQUIRE(calc("-3 + 7") == "4");
    REQUIRE(calc("-(2 + 3)") == "-5");
}

TEST_CASE("Calculator: floating point", "[calc]") {
    REQUIRE(calc("1.5 + 2.5") == "4");
    auto result = std::stod(calc("10 / 3"));
    REQUIRE(std::abs(result - 3.333333) < 0.001);
}

TEST_CASE("Calculator: combined expression", "[calc]") {
    REQUIRE(calc("2^10 + 3*17") == "1075");
}

TEST_CASE("Calculator: division by zero", "[calc]") {
    REQUIRE_THROWS_AS(calc("1/0"), std::runtime_error);
}

TEST_CASE("Calculator: invalid expression", "[calc]") {
    REQUIRE_THROWS(calc(""));
    REQUIRE_THROWS(calc("abc"));
    REQUIRE_THROWS(calc("2 +"));
}
