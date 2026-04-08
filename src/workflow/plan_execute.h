#pragma once
// Plan-and-Execute workflow: plan steps, execute each via ReAct, synthesize.

#include "core/thread_pool.h"
#include "llm/llm_client_interface.h"
#include "session/session.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/react.h"
#include "workflow/workflow.h"

#include <string>
#include <vector>

namespace forge {

class PlanExecuteWorkflow : public Workflow {
public:
    PlanExecuteWorkflow(ILLMClient& llm, ToolExecutor& executor,
                        const ToolRegistry& registry, ThreadPool& pool)
        : llm_(llm), executor_(executor), registry_(registry), pool_(pool) {}

    WorkflowResult run(Session& session) override;

    std::string name() const override { return "plan-execute"; }

private:
    struct Step {
        int id;
        std::string description;
        std::string result;
    };

    std::vector<Step> generate_plan(Session& session, WorkflowResult& result);
    std::string synthesize(Session& session, const std::vector<Step>& steps,
                           WorkflowResult& result);

    ILLMClient& llm_;
    ToolExecutor& executor_;
    const ToolRegistry& registry_;
    ThreadPool& pool_;
};

}  // namespace forge
