#pragma once
// Web search tool: placeholder that returns canned results.
// Real implementation requires a search API key (SerpAPI, Brave, etc).

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <string>

namespace forge {
namespace builtin {

/// Register the web_search tool (placeholder).
inline void register_web_search(ToolRegistry& registry) {
    ToolSpec spec;
    spec.name = "web_search";
    spec.description = "Search the web for information. Returns a summary of top results.";
    spec.parameters = Json::parse(R"({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "The search query"
            }
        },
        "required": ["query"]
    })");
    spec.timeout = std::chrono::milliseconds(15000);

    registry.register_tool(std::move(spec), [](const std::string& args_json) -> std::string {
        auto j = Json::parse(args_json);
        std::string query = j.value("query", "");
        if (query.empty()) {
            throw std::runtime_error("Missing 'query' parameter");
        }

        // Placeholder response — no real search API configured.
        return "[web_search] No search API configured. "
               "Query was: \"" + query + "\". "
               "To enable real search, configure a SerpAPI or Brave Search API key. "
               "For now, try using other tools to answer the question.";
    });
}

}  // namespace builtin
}  // namespace forge
