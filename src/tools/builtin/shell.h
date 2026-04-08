#pragma once
// Shell tool: runs an allowlisted shell command and captures output.
// Security: only commands on the allowlist can be executed.

#include "tools/tool_spec.h"
#include "utils/json.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

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

            // Security: reject ALL shell metacharacters that could escape the allowlist.
            for (char c : command) {
                if (c == ';' || c == '&' || c == '|' || c == '`' ||
                    c == '$' || c == '\n' || c == '\r' || c == '>' ||
                    c == '<' || c == '(' || c == ')' || c == '{' ||
                    c == '}' || c == '!' || c == '\\' || c == '\'' ||
                    c == '"') {
                    throw std::runtime_error(
                        "Shell metacharacter '" + std::string(1, c) +
                        "' not allowed in commands");
                }
            }

            // Tokenize the command into argv for execvp (no shell involved).
            std::vector<std::string> tokens;
            {
                std::istringstream iss(command);
                std::string token;
                while (iss >> token) {
                    tokens.push_back(token);
                }
            }
            if (tokens.empty()) {
                throw std::runtime_error("Empty command after parsing");
            }

            // Build argv array for execvp.
            std::vector<char*> argv;
            for (auto& t : tokens) {
                argv.push_back(t.data());
            }
            argv.push_back(nullptr);

            // Create pipe for capturing stdout+stderr.
            int pipefd[2];
            if (pipe(pipefd) != 0) {
                throw std::runtime_error("Failed to create pipe");
            }

            pid_t pid = fork();
            if (pid < 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                throw std::runtime_error("Failed to fork");
            }

            if (pid == 0) {
                // Child: redirect stdout and stderr to the pipe write end.
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);

                execvp(argv[0], argv.data());
                // If execvp returns, it failed.
                _exit(127);
            }

            // Parent: read from pipe read end.
            close(pipefd[1]);

            std::string output;
            std::array<char, 4096> buf;
            ssize_t n;
            while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
                output.append(buf.data(), static_cast<size_t>(n));
            }
            close(pipefd[0]);

            int status = 0;
            waitpid(pid, &status, 0);

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
