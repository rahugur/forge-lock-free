#pragma once
// ReAct workflow: Thought → Action → Observation → repeat.
// The LLM generates tool calls; we execute them and loop.

#include "llm/llm_client_interface.h"
#include "session/session.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"
#include "workflow/workflow.h"

#include <string>

namespace forge {

class ReActWorkflow : public Workflow {
public:
    ReActWorkflow(ILLMClient& llm, ToolExecutor& executor,
                  const ToolRegistry& registry)
        : llm_(llm), executor_(executor), registry_(registry) {}

    WorkflowResult run(Session& session) override;

    std::string name() const override { return "react"; }

private:
    ILLMClient& llm_;
    ToolExecutor& executor_;
    const ToolRegistry& registry_;
};

}  // namespace forge
