#pragma once
// Tool specification and handler types for the Forge runtime.

#include "utils/json.h"

#include <chrono>
#include <functional>
#include <string>

namespace forge {

/// Specification of a tool that an LLM agent can invoke.
struct ToolSpec {
    std::string name;
    std::string description;
    Json parameters;  // JSON Schema for the tool's parameters

    std::chrono::milliseconds timeout{30000};
    bool allow_parallel = true;   // can run alongside other tools?
    bool idempotent = false;      // safe to retry on failure?
};

/// A tool handler: takes JSON arguments string, returns output string.
/// Throws on error.
using ToolHandler = std::function<std::string(const std::string& arguments_json)>;

/// A registered tool: spec + handler.
struct RegisteredTool {
    ToolSpec spec;
    ToolHandler handler;
};

}  // namespace forge
