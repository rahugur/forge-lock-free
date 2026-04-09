#pragma once
// Abstract LLM client interface.
// Allows workflows to work with both direct and throttled LLM clients.

#include "core/future.h"
#include "llm/message.h"
#include "utils/json.h"

namespace forge {

struct LLMClientConfig;

class ILLMClient {
public:
    virtual ~ILLMClient() = default;

    /// Send a chat completion request.
    virtual Future<LLMResponse> complete(const vortex::Json& messages,
                                         const vortex::Json& tools = Json()) = 0;

    /// Access the client configuration.
    virtual const LLMClientConfig& config() const = 0;
};

}  // namespace forge
