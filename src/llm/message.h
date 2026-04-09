#pragma once
// Shared message types for the Forge runtime.
// Used by the LLM client, session, workflow, and tool executor.

#include "utils/json.h"

#include <chrono>
#include <string>
#include <vector>

namespace forge {

/// A single tool call requested by the LLM.
struct ToolCall {
    std::string id;              // unique call id (e.g. "call_abc123")
    std::string name;            // tool name (e.g. "calculator")
    std::string arguments_json;  // raw JSON string of arguments
};

/// Result of executing a tool call.
struct ToolResult {
    std::string tool_call_id;    // matches ToolCall::id
    std::string output;          // tool output (string)
    bool is_error = false;       // true if the tool failed
};

/// A conversation message (OpenAI-compatible roles).
struct Message {
    std::string role;            // "system", "user", "assistant", "tool"
    std::string content;         // text content (may be empty for tool-call messages)
    std::vector<ToolCall> tool_calls;  // present when role == "assistant"
    std::string tool_call_id;    // present when role == "tool"

    bool has_tool_calls() const { return !tool_calls.empty(); }

    bool has_content() const { return !content.empty(); }

    /// Serialize to OpenAI JSON format.
    vortex::Json to_json() const {
        vortex::Json j;
        j["role"] = role;

        if (!content.empty()) {
            j["content"] = content;
        } else if (role != "assistant" || tool_calls.empty()) {
            // OpenAI requires content field even if null for some roles.
            j["content"] = nullptr;
        }

        if (!tool_calls.empty()) {
            Json calls = Json::array();
            for (auto& tc : tool_calls) {
                calls.push_back({
                    {"id", tc.id},
                    {"type", "function"},
                    {"function", {
                        {"name", tc.name},
                        {"arguments", tc.arguments_json}
                    }}
                });
            }
            j["tool_calls"] = calls;
        }

        if (!tool_call_id.empty()) {
            j["tool_call_id"] = tool_call_id;
        }

        return j;
    }

    /// Parse from OpenAI JSON response.
    static Message from_json(const vortex::Json& j) {
        Message msg;
        msg.role = j.value("role", "");
        if (j.contains("content") && !j["content"].is_null()) {
            msg.content = j["content"].get<std::string>();
        }
        if (j.contains("tool_calls")) {
            for (auto& tc : j["tool_calls"]) {
                ToolCall call;
                call.id = tc.value("id", "");
                if (tc.contains("function")) {
                    call.name = tc["function"].value("name", "");
                    call.arguments_json = tc["function"].value("arguments", "");
                }
                msg.tool_calls.push_back(std::move(call));
            }
        }
        if (j.contains("tool_call_id")) {
            msg.tool_call_id = j["tool_call_id"].get<std::string>();
        }
        return msg;
    }
};

/// Response from an LLM completion call.
struct LLMResponse {
    Message message;             // the assistant's reply
    int prompt_tokens = 0;       // input token count
    int completion_tokens = 0;   // output token count
    std::string model;           // model that was used
    std::string error;           // non-empty on failure
};

}  // namespace forge
