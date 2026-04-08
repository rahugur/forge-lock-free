#pragma once
// LLM client: OpenAI-compatible, non-streaming (Phase 1).
// Uses AsyncHttpClient to POST to /v1/chat/completions.

#include "core/async_http_client.h"
#include "core/future.h"
#include "llm/llm_client_interface.h"
#include "llm/message.h"
#include "tools/tool_registry.h"
#include "utils/json.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace forge {

struct LLMClientConfig {
    std::string api_base = "https://api.openai.com";
    std::string model = "gpt-4o";
    std::string api_key;  // read from env if empty
    double temperature = 0.0;
    int max_tokens = 4096;
};

class LLMClient : public ILLMClient {
public:
    LLMClient(AsyncHttpClient& http, LLMClientConfig cfg)
        : http_(http), cfg_(std::move(cfg)) {
        // Resolve API key from environment if not set.
        if (cfg_.api_key.empty()) {
            const char* env = std::getenv("OPENAI_API_KEY");
            if (env) cfg_.api_key = env;
        }
    }

    /// Send a chat completion request.
    /// messages: conversation history (JSON array).
    /// tools: optional OpenAI tools array.
    /// Returns a future that resolves to the LLM response.
    Future<LLMResponse> complete(const Json& messages,
                                 const Json& tools = Json()) override {
        Json body;
        body["model"] = cfg_.model;
        body["messages"] = messages;
        body["temperature"] = cfg_.temperature;
        body["max_tokens"] = cfg_.max_tokens;

        if (!tools.empty() && tools.is_array()) {
            body["tools"] = tools;
            body["tool_choice"] = "auto";
        }

        std::string url = cfg_.api_base + "/v1/chat/completions";
        std::string payload = body.dump();

        std::unordered_map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + cfg_.api_key}
        };

        auto http_future = http_.post(url, payload, "application/json", headers);
        auto model = cfg_.model;

        // Transform HttpResponse → LLMResponse.
        return http_.pool().submit(
            [fut = std::move(http_future), model]() mutable -> LLMResponse {
            auto http_resp = fut.get();

            LLMResponse response;
            if (!http_resp.error.empty()) {
                response.error = "HTTP error: " + http_resp.error;
                return response;
            }

            if (http_resp.status != 200) {
                response.error = "API error (HTTP " +
                    std::to_string(http_resp.status) + "): " + http_resp.body;
                return response;
            }

            try {
                auto j = Json::parse(http_resp.body);

                if (j.contains("error")) {
                    response.error = j["error"].value("message", "Unknown API error");
                    return response;
                }

                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    if (choice.contains("message")) {
                        response.message = Message::from_json(choice["message"]);
                    }
                }

                if (j.contains("usage")) {
                    response.prompt_tokens = j["usage"].value("prompt_tokens", 0);
                    response.completion_tokens = j["usage"].value("completion_tokens", 0);
                }

                response.model = j.value("model", model);
            } catch (const std::exception& e) {
                response.error = std::string("Failed to parse API response: ") + e.what();
            }

            return response;
        });
    }

    const LLMClientConfig& config() const override { return cfg_; }

private:
    AsyncHttpClient& http_;
    LLMClientConfig cfg_;
};

}  // namespace forge
