#pragma once
// Task DAG: topological execution of dependent tool calls.
// Nodes are tool calls with dependencies; execution proceeds level-by-level.

#include "core/thread_pool.h"
#include "llm/message.h"
#include "tools/tool_executor.h"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge {

struct DAGNode {
    ToolCall call;
    std::vector<size_t> dependencies;
};

class TaskDAG {
public:
    size_t add_node(ToolCall call, std::vector<size_t> deps = {}) {
        size_t id = nodes_.size();
        for (auto d : deps) {
            if (d >= id) {
                throw std::invalid_argument("Dependency on future node");
            }
        }
        nodes_.push_back({std::move(call), std::move(deps)});
        return id;
    }

    size_t size() const { return nodes_.size(); }

    std::vector<ToolResult> execute(ToolExecutor& executor, ThreadPool& pool) {
        auto levels = topological_sort();
        std::unordered_map<size_t, std::string> results;
        std::vector<ToolResult> all_results;
        all_results.reserve(nodes_.size());

        for (auto& level : levels) {
            // Substitute placeholders in arguments.
            std::vector<ToolCall> calls;
            calls.reserve(level.size());
            for (auto idx : level) {
                ToolCall call = nodes_[idx].call;
                call.arguments_json = substitute(call.arguments_json, results);
                calls.push_back(std::move(call));
            }

            // Execute this level in parallel.
            auto level_results = executor.execute_batch(calls);

            // Store results for placeholder substitution.
            for (size_t i = 0; i < level.size(); ++i) {
                results[level[i]] = level_results[i].output;
                all_results.push_back(std::move(level_results[i]));
            }
        }

        return all_results;
    }

private:
    // Kahn's algorithm: returns groups of nodes by level.
    std::vector<std::vector<size_t>> topological_sort() const {
        size_t n = nodes_.size();
        std::vector<int> in_degree(n, 0);
        std::vector<std::vector<size_t>> dependents(n);

        for (size_t i = 0; i < n; ++i) {
            for (auto dep : nodes_[i].dependencies) {
                dependents[dep].push_back(i);
                ++in_degree[i];
            }
        }

        std::queue<size_t> ready;
        for (size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) ready.push(i);
        }

        std::vector<std::vector<size_t>> levels;
        size_t processed = 0;

        while (!ready.empty()) {
            std::vector<size_t> level;
            size_t level_size = ready.size();
            for (size_t i = 0; i < level_size; ++i) {
                auto node = ready.front();
                ready.pop();
                level.push_back(node);
                ++processed;

                for (auto dep : dependents[node]) {
                    if (--in_degree[dep] == 0) {
                        ready.push(dep);
                    }
                }
            }
            levels.push_back(std::move(level));
        }

        if (processed != n) {
            throw std::runtime_error("Cycle detected in task DAG");
        }

        return levels;
    }

    // Replace {{node_N_result}} placeholders with actual results.
    static std::string substitute(const std::string& input,
                                  const std::unordered_map<size_t, std::string>& results) {
        static const std::regex placeholder_re(R"(\{\{node_(\d+)_result\}\})");
        std::string output;
        std::sregex_iterator it(input.begin(), input.end(), placeholder_re);
        std::sregex_iterator end;
        size_t last_pos = 0;

        for (; it != end; ++it) {
            auto& match = *it;
            output.append(input, last_pos, match.position() - last_pos);

            size_t node_id = std::stoull(match[1].str());
            auto rit = results.find(node_id);
            if (rit != results.end()) {
                output.append(rit->second);
            } else {
                output.append(match[0].str());  // keep placeholder if not found
            }
            last_pos = match.position() + match.length();
        }
        output.append(input, last_pos, std::string::npos);
        return output;
    }

    std::vector<DAGNode> nodes_;
};

}  // namespace forge
