#pragma once
// Workflow base class: defines the interface for agent execution patterns.

#include "session/session.h"

#include <string>

namespace forge {

/// Result of running a workflow.
struct WorkflowResult {
    bool success = false;
    std::string answer;       // final answer (if success)
    std::string error;        // error description (if !success)
    int steps_taken = 0;
    int total_prompt_tokens = 0;
    int total_completion_tokens = 0;
};

/// Abstract base class for agent workflow patterns.
class Workflow {
public:
    virtual ~Workflow() = default;

    /// Run the workflow on the given session.  Blocks until complete.
    virtual WorkflowResult run(Session& session) = 0;

    /// Human-readable name of the workflow pattern.
    virtual std::string name() const = 0;
};

}  // namespace forge
