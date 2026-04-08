#pragma once
// Tool registry: register handlers, lookup by name, serialize for LLM.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge {

class ToolRegistry {
public:
    /// Register a tool with its handler.
    void register_tool(ToolSpec spec, ToolHandler handler) {
        std::string name = spec.name;
        tools_[name] = RegisteredTool{std::move(spec), std::move(handler)};
    }

    /// Look up a registered tool by name.  Throws if not found.
    const RegisteredTool& get(const std::string& name) const {
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            throw std::runtime_error("Unknown tool: " + name);
        }
        return it->second;
    }

    /// Check if a tool exists.
    bool has(const std::string& name) const {
        return tools_.count(name) > 0;
    }

    /// Serialize all tools to OpenAI "tools" array format for the API request.
    Json to_openai_tools_json() const {
        Json arr = Json::array();
        for (auto& [name, tool] : tools_) {
            arr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool.spec.name},
                    {"description", tool.spec.description},
                    {"parameters", tool.spec.parameters}
                }}
            });
        }
        return arr;
    }

    /// List all registered tool names.
    std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(tools_.size());
        for (auto& [name, _] : tools_) {
            result.push_back(name);
        }
        return result;
    }

    /// Number of registered tools.
    size_t size() const { return tools_.size(); }

    bool empty() const { return tools_.empty(); }

    /// Load tool selection from a JSON file.
    /// The file lists tool names to enable (must be pre-registered as builtins).
    /// Format: { "tools": ["calculator", "file_read", ...] }
    std::vector<std::string> load_selection(const std::string& path) const {
        auto j = json_util::parse_file(path);
        std::vector<std::string> selected;
        if (j.contains("tools") && j["tools"].is_array()) {
            for (auto& name : j["tools"]) {
                auto n = name.get<std::string>();
                if (!has(n)) {
                    throw std::runtime_error(
                        "Tool '" + n + "' listed in " + path + " is not registered");
                }
                selected.push_back(std::move(n));
            }
        }
        return selected;
    }

private:
    std::unordered_map<std::string, RegisteredTool> tools_;
};

}  // namespace forge
