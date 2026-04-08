#pragma once
// File read tool: reads a file and returns its contents.
// Sandboxed: rejects paths containing ".." or absolute paths outside a sandbox.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace forge {
namespace builtin {

/// Register the file_read tool.
/// sandbox_dir: if non-empty, only allow reading files under this directory.
inline void register_file_read(ToolRegistry& registry,
                               const std::string& sandbox_dir = ".") {
    ToolSpec spec;
    spec.name = "file_read";
    spec.description = "Read the contents of a text file. Paths are relative to the working directory.";
    spec.parameters = Json::parse(R"json({
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "File path to read (relative to working directory)"
            },
            "max_lines": {
                "type": "integer",
                "description": "Maximum number of lines to read (default: 200)"
            }
        },
        "required": ["path"]
    })json");
    spec.timeout = std::chrono::milliseconds(5000);

    registry.register_tool(std::move(spec),
        [sandbox_dir](const std::string& args_json) -> std::string {
            auto j = Json::parse(args_json);
            std::string path = j.value("path", "");
            int max_lines = j.value("max_lines", 200);

            if (path.empty()) {
                throw std::runtime_error("Missing 'path' parameter");
            }

            // Security: reject directory traversal.
            if (path.find("..") != std::string::npos) {
                throw std::runtime_error("Directory traversal not allowed: " + path);
            }

            // Security: reject absolute paths (unless under sandbox).
            if (!path.empty() && path[0] == '/') {
                if (sandbox_dir.empty() || path.find(sandbox_dir) != 0) {
                    throw std::runtime_error("Absolute paths not allowed: " + path);
                }
            }

            // Prepend sandbox dir if path is relative.
            std::string full_path = path;
            if (!sandbox_dir.empty() && path[0] != '/') {
                full_path = sandbox_dir + "/" + path;
            }

            std::ifstream ifs(full_path);
            if (!ifs.is_open()) {
                throw std::runtime_error("Cannot open file: " + full_path);
            }

            std::string result;
            std::string line;
            int count = 0;
            while (std::getline(ifs, line) && count < max_lines) {
                result += line + "\n";
                ++count;
            }

            if (count == max_lines && !ifs.eof()) {
                result += "\n... (truncated at " + std::to_string(max_lines) + " lines)\n";
            }

            return result;
        });
}

}  // namespace builtin
}  // namespace forge
