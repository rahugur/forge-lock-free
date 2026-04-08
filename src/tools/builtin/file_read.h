#pragma once
// File read tool: reads a file and returns its contents.
// Sandboxed: rejects paths containing ".." or absolute paths outside a sandbox.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <filesystem>
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

            // Security: resolve the canonical sandbox directory.
            std::filesystem::path sandbox_canonical;
            if (!sandbox_dir.empty()) {
                std::error_code ec;
                sandbox_canonical = std::filesystem::canonical(sandbox_dir, ec);
                if (ec) {
                    throw std::runtime_error("Invalid sandbox directory: " + sandbox_dir);
                }
            }

            // Build the full path: relative paths are resolved against the sandbox.
            std::filesystem::path requested;
            if (!sandbox_dir.empty() && path[0] != '/') {
                requested = sandbox_canonical / path;
            } else {
                requested = path;
            }

            // Resolve to canonical form (follows symlinks, resolves ..).
            std::error_code ec;
            std::filesystem::path canonical_path = std::filesystem::canonical(requested, ec);
            if (ec) {
                throw std::runtime_error("Cannot resolve path: " + path);
            }

            // Security: verify the canonical path is under the sandbox.
            if (!sandbox_dir.empty()) {
                std::string canonical_str = canonical_path.string();
                std::string sandbox_str = sandbox_canonical.string();
                // The canonical path must start with sandbox path followed by '/' (or be exact match).
                if (canonical_str != sandbox_str &&
                    (canonical_str.size() <= sandbox_str.size() ||
                     canonical_str.compare(0, sandbox_str.size(), sandbox_str) != 0 ||
                     canonical_str[sandbox_str.size()] != '/')) {
                    throw std::runtime_error("Path escapes sandbox: " + path);
                }
            }

            std::string full_path = canonical_path.string();

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
