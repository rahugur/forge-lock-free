#pragma once
// Tool executor: dispatches tool calls on the thread pool with timeout.

#include "core/future.h"
#include "core/thread_pool.h"
#include "llm/message.h"
#include "tools/tool_registry.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace forge {

class ToolExecutor {
public:
    ToolExecutor(ThreadPool& pool, const ToolRegistry& registry)
        : pool_(pool), registry_(registry) {}

    /// Execute a single tool call.  Blocks until completion or timeout.
    ToolResult execute(const ToolCall& call) {
        if (!registry_.has(call.name)) {
            return {call.id, "Unknown tool: " + call.name, /*is_error=*/true};
        }

        auto& tool = registry_.get(call.name);
        auto handler = tool.handler;
        auto args = call.arguments_json;
        auto call_id = call.id;

        auto fut = pool_.submit([handler, args]() -> std::string {
            return handler(args);
        });

        try {
            auto result = fut.wait_for(tool.spec.timeout);
            if (!result.has_value()) {
                spdlog::warn("Tool '{}' timed out after {}ms",
                             call.name, tool.spec.timeout.count());
                return {call_id, "Tool timed out", /*is_error=*/true};
            }
            return {call_id, *result, /*is_error=*/false};
        } catch (const std::exception& e) {
            return {call_id, std::string("Tool error: ") + e.what(), /*is_error=*/true};
        }
    }

    /// Execute multiple tool calls.  Runs them concurrently on the pool,
    /// then collects results.
    std::vector<ToolResult> execute_batch(const std::vector<ToolCall>& calls) {
        // Launch all concurrently.
        struct PendingCall {
            std::string call_id;
            std::string name;
            Future<std::string> future;
            std::chrono::milliseconds timeout;
        };

        std::vector<PendingCall> pending;
        pending.reserve(calls.size());

        for (auto& call : calls) {
            if (!registry_.has(call.name)) {
                // Will handle below when collecting results.
                pending.push_back({call.id, call.name, {}, std::chrono::milliseconds(0)});
                continue;
            }

            auto& tool = registry_.get(call.name);
            auto handler = tool.handler;
            auto args = call.arguments_json;

            auto fut = pool_.submit([handler, args]() -> std::string {
                return handler(args);
            });

            pending.push_back({call.id, call.name, std::move(fut), tool.spec.timeout});
        }

        // Collect results.
        std::vector<ToolResult> results;
        results.reserve(pending.size());

        for (auto& p : pending) {
            if (!p.future.valid()) {
                results.push_back({p.call_id, "Unknown tool: " + p.name, true});
                continue;
            }

            try {
                auto result = p.future.wait_for(p.timeout);
                if (!result.has_value()) {
                    spdlog::warn("Tool '{}' timed out after {}ms",
                                 p.name, p.timeout.count());
                    results.push_back({p.call_id, "Tool timed out", true});
                } else {
                    results.push_back({p.call_id, *result, false});
                }
            } catch (const std::exception& e) {
                results.push_back({p.call_id, std::string("Tool error: ") + e.what(), true});
            }
        }

        return results;
    }

private:
    ThreadPool& pool_;
    const ToolRegistry& registry_;
};

}  // namespace forge
