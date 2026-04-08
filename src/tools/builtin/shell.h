#pragma once
// Shell tool: runs an allowlisted shell command and captures output.
// Security: only commands on the allowlist can be executed.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge {
namespace builtin {

/// Register the shell tool.
/// allowed_commands: list of command prefixes that are permitted (e.g. {"ls", "cat", "grep"}).
/// If empty, all commands are allowed (DANGEROUS — only for trusted environments).
inline void register_shell(ToolRegistry& registry,
                           std::vector<std::string> allowed_commands = {
                               "ls", "cat", "head", "tail", "wc", "grep",
                               "find", "date", "echo", "pwd", "whoami"
                           }) {
    ToolSpec spec;
    spec.name = "shell";
    spec.description = "Run a shell command and return stdout/stderr. "
                       "Only allowlisted commands are permitted.";
    spec.parameters = Json::parse(R"({
        "type": "object",
        "properties": {
            "command": {
                "type": "string",
                "description": "The shell command to execute"
            }
        },
        "required": ["command"]
    })");
    spec.timeout = std::chrono::milliseconds(10000);

    registry.register_tool(std::move(spec),
        [allowed_commands = std::move(allowed_commands)](const std::string& args_json) -> std::string {
            auto j = Json::parse(args_json);
            std::string command = j.value("command", "");

            if (command.empty()) {
                throw std::runtime_error("Missing 'command' parameter");
            }

            // Security: check against allowlist.
            if (!allowed_commands.empty()) {
                bool allowed = false;
                for (auto& prefix : allowed_commands) {
                    if (command.compare(0, prefix.size(), prefix) == 0) {
                        // Ensure the prefix is followed by space, end, or flag.
                        if (command.size() == prefix.size() ||
                            command[prefix.size()] == ' ' ||
                            command[prefix.size()] == '\t') {
                            allowed = true;
                            break;
                        }
                    }
                }
                if (!allowed) {
                    throw std::runtime_error(
                        "Command not allowed. Permitted: " +
                        [&]() {
                            std::string s;
                            for (size_t i = 0; i < allowed_commands.size(); ++i) {
                                if (i > 0) s += ", ";
                                s += allowed_commands[i];
                            }
                            return s;
                        }());
                }
            }

            // Security: reject shell metacharacters that could escape the allowlist.
            for (char c : command) {
                if (c == ';' || c == '&' || c == '|' || c == '`' ||
                    c == '$' || c == '\n' || c == '\r') {
                    throw std::runtime_error(
                        "Shell metacharacter '" + std::string(1, c) +
                        "' not allowed in commands");
                }
            }

            // Redirect stderr to stdout.
            std::string cmd = command + " 2>&1";
            std::unique_ptr<FILE, int(*)(FILE*)> pipe(
                popen(cmd.c_str(), "r"), pclose);

            if (!pipe) {
                throw std::runtime_error("Failed to execute command");
            }

            std::string output;
            std::array<char, 4096> buf;
            while (fgets(buf.data(), buf.size(), pipe.get()) != nullptr) {
                output += buf.data();
            }

            // Truncate very long output.
            constexpr size_t MAX_OUTPUT = 50000;
            if (output.size() > MAX_OUTPUT) {
                output.resize(MAX_OUTPUT);
                output += "\n... (output truncated at 50KB)\n";
            }

            return output;
        });
}

}  // namespace builtin
}  // namespace forge
