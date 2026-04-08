#pragma once
// ScriptedMockLLM: in-process mock LLM that returns pre-configured responses.
// No HTTP server needed — implements ILLMClient directly.

#include "core/future.h"
#include "llm/llm_client.h"
#include "llm/llm_client_interface.h"
#include "llm/message.h"
#include "utils/json.h"

#include <functional>
#include <mutex>
#include <queue>
#include <string>

namespace forge::testing {

class ScriptedMockLLM : public ILLMClient {
public:
    /// Add a response to the queue. Responses are returned in FIFO order.
    void add_response(LLMResponse response) {
        std::lock_guard lock(mu_);
        responses_.push(std::move(response));
    }

    /// Add a simple text response.
    void add_text_response(const std::string& content,
                           int prompt_tokens = 10, int completion_tokens = 5) {
        LLMResponse resp;
        resp.message.role = "assistant";
        resp.message.content = content;
        resp.prompt_tokens = prompt_tokens;
        resp.completion_tokens = completion_tokens;
        add_response(std::move(resp));
    }

    /// Add a tool call response.
    void add_tool_call_response(const std::string& tool_name,
                                const std::string& args_json,
                                const std::string& call_id = "call_1") {
        LLMResponse resp;
        resp.message.role = "assistant";
        ToolCall tc;
        tc.id = call_id;
        tc.name = tool_name;
        tc.arguments_json = args_json;
        resp.message.tool_calls.push_back(std::move(tc));
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;
        add_response(std::move(resp));
    }

    /// Add an error response.
    void add_error_response(const std::string& error) {
        LLMResponse resp;
        resp.error = error;
        add_response(std::move(resp));
    }

    /// Set a handler to be called for every request (overrides queue).
    void set_handler(std::function<LLMResponse(const Json&, const Json&)> handler) {
        std::lock_guard lock(mu_);
        handler_ = std::move(handler);
    }

    int call_count() const {
        std::lock_guard lock(mu_);
        return call_count_;
    }

    Future<LLMResponse> complete(const Json& messages,
                                  const Json& tools = Json()) override {
        Promise<LLMResponse> promise;
        auto future = promise.get_future();

        LLMResponse resp;
        {
            std::lock_guard lock(mu_);
            ++call_count_;

            if (handler_) {
                resp = handler_(messages, tools);
            } else if (!responses_.empty()) {
                resp = std::move(responses_.front());
                responses_.pop();
            } else {
                resp.message.role = "assistant";
                resp.message.content = "Default mock response";
                resp.prompt_tokens = 10;
                resp.completion_tokens = 5;
            }
        }

        promise.set_value(std::move(resp));
        return future;
    }

    const LLMClientConfig& config() const override {
        static LLMClientConfig cfg;
        return cfg;
    }

private:
    mutable std::mutex mu_;
    std::queue<LLMResponse> responses_;
    std::function<LLMResponse(const Json&, const Json&)> handler_;
    int call_count_ = 0;
};

}  // namespace forge::testing
