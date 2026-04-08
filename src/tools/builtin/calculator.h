#pragma once
// Calculator tool: evaluates simple math expressions.
// Recursive-descent parser for +, -, *, /, ^, parentheses, unary minus.
// No eval() or system() — pure C++ parsing for security.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace forge {
namespace builtin {

namespace detail {

class ExprParser {
public:
    explicit ExprParser(const std::string& input) : input_(input), pos_(0) {}

    double parse() {
        double result = parse_expr();
        skip_ws();
        if (pos_ < input_.size()) {
            throw std::runtime_error(
                "Unexpected character at position " + std::to_string(pos_));
        }
        return result;
    }

private:
    void skip_ws() {
        while (pos_ < input_.size() && input_[pos_] == ' ') ++pos_;
    }

    char peek() {
        skip_ws();
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    char consume() {
        skip_ws();
        return pos_ < input_.size() ? input_[pos_++] : '\0';
    }

    // expr = term (('+' | '-') term)*
    double parse_expr() {
        double left = parse_term();
        while (true) {
            char op = peek();
            if (op == '+' || op == '-') {
                consume();
                double right = parse_term();
                left = (op == '+') ? left + right : left - right;
            } else {
                break;
            }
        }
        return left;
    }

    // term = power (('*' | '/') power)*
    double parse_term() {
        double left = parse_power();
        while (true) {
            char op = peek();
            if (op == '*' || op == '/') {
                consume();
                double right = parse_power();
                if (op == '/') {
                    if (right == 0.0) throw std::runtime_error("Division by zero");
                    left /= right;
                } else {
                    left *= right;
                }
            } else {
                break;
            }
        }
        return left;
    }

    // power = unary ('^' power)?   (right-associative)
    double parse_power() {
        double base = parse_unary();
        if (peek() == '^') {
            consume();
            double exp = parse_power();
            return std::pow(base, exp);
        }
        return base;
    }

    // unary = '-' unary | atom
    double parse_unary() {
        if (peek() == '-') {
            consume();
            return -parse_unary();
        }
        return parse_atom();
    }

    // atom = '(' expr ')' | number
    double parse_atom() {
        if (peek() == '(') {
            consume();
            double val = parse_expr();
            if (consume() != ')') {
                throw std::runtime_error("Missing closing parenthesis");
            }
            return val;
        }
        return parse_number();
    }

    double parse_number() {
        skip_ws();
        size_t start = pos_;
        if (pos_ < input_.size() && (input_[pos_] == '-' || input_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < input_.size() && (std::isdigit(input_[pos_]) || input_[pos_] == '.')) {
            ++pos_;
        }
        // Handle scientific notation
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ == start) {
            throw std::runtime_error(
                "Expected number at position " + std::to_string(pos_));
        }
        return std::stod(input_.substr(start, pos_ - start));
    }

    std::string input_;
    size_t pos_;
};

}  // namespace detail

/// Register the calculator tool.
inline void register_calculator(ToolRegistry& registry) {
    ToolSpec spec;
    spec.name = "calculator";
    spec.description = "Evaluate a mathematical expression. Supports +, -, *, /, ^ (power), parentheses.";
    spec.parameters = Json::parse(R"({
        "type": "object",
        "properties": {
            "expression": {
                "type": "string",
                "description": "The math expression to evaluate, e.g. '2^10 + 3*17'"
            }
        },
        "required": ["expression"]
    })");
    spec.timeout = std::chrono::milliseconds(1000);

    registry.register_tool(std::move(spec), [](const std::string& args_json) -> std::string {
        auto j = Json::parse(args_json);
        std::string expr = j.value("expression", "");
        if (expr.empty()) {
            throw std::runtime_error("Missing 'expression' parameter");
        }
        double result = detail::ExprParser(expr).parse();
        // Format: avoid trailing zeros for integers.
        if (result == std::floor(result) && std::abs(result) < 1e15) {
            return std::to_string(static_cast<long long>(result));
        }
        return std::to_string(result);
    });
}

}  // namespace builtin
}  // namespace forge
