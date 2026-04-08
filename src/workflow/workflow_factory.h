#pragma once
// Workflow factory: maps workflow names to constructor functions.
// Allows SessionManager to create any registered workflow by name.

#include "core/thread_pool.h"
#include "llm/llm_client_interface.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/workflow.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge {

using WorkflowConstructor = std::function<std::unique_ptr<Workflow>(
    ILLMClient& llm, ToolExecutor& executor,
    const ToolRegistry& registry, ThreadPool& pool)>;

class WorkflowFactory {
public:
    void register_workflow(const std::string& name, WorkflowConstructor ctor) {
        registry_[name] = std::move(ctor);
    }

    std::unique_ptr<Workflow> create(const std::string& name,
                                      ILLMClient& llm,
                                      ToolExecutor& executor,
                                      const ToolRegistry& registry,
                                      ThreadPool& pool) const {
        auto it = registry_.find(name);
        if (it == registry_.end()) {
            throw std::invalid_argument("Unknown workflow: " + name);
        }
        return it->second(llm, executor, registry, pool);
    }

    bool has(const std::string& name) const {
        return registry_.count(name) > 0;
    }

    std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(registry_.size());
        for (auto& [k, v] : registry_) {
            result.push_back(k);
        }
        return result;
    }

private:
    std::unordered_map<std::string, WorkflowConstructor> registry_;
};

}  // namespace forge
