#pragma once
// Map-Reduce workflow: decompose into sub-tasks, execute in parallel, aggregate.

#include "core/thread_pool.h"
#include "llm/llm_client_interface.h"
#include "session/session.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/workflow.h"

#include <string>
#include <vector>

namespace forge {

class MapReduceWorkflow : public Workflow {
public:
    MapReduceWorkflow(ILLMClient& llm, ToolExecutor& executor,
                      const ToolRegistry& registry, ThreadPool& pool)
        : llm_(llm), executor_(executor), registry_(registry), pool_(pool) {}

    WorkflowResult run(Session& session) override;

    std::string name() const override { return "map-reduce"; }

private:
    struct SubTask {
        int id;
        std::string description;
        std::string result;
        bool success = false;
    };

    std::vector<SubTask> decompose(Session& session, WorkflowResult& result);
    std::string reduce(Session& session, const std::vector<SubTask>& tasks,
                       WorkflowResult& result);

    ILLMClient& llm_;
    ToolExecutor& executor_;
    const ToolRegistry& registry_;
    ThreadPool& pool_;
};

}  // namespace forge
