#pragma once
// Throttled LLM client decorator.
// Wraps an ILLMClient with semaphore (concurrency cap) and rate limiter.
// The semaphore is held for the duration of the LLM HTTP call, released
// asynchronously via Future::then() when the response arrives.

#include "core/future.h"
#include "core/rate_limiter.h"
#include "core/semaphore.h"
#include "llm/llm_client_interface.h"
#include "llm/message.h"
#include "utils/json.h"

#include <memory>

namespace forge {

class ThrottledLLMClient : public ILLMClient {
public:
    ThrottledLLMClient(ILLMClient& inner, Semaphore& sem, RateLimiter& rate)
        : inner_(inner), sem_(sem), rate_(rate) {}

    /// Acquires rate limit and semaphore permit before delegating.
    /// The semaphore permit is released asynchronously when the inner
    /// LLM call completes (via Future::then), so it tracks actual
    /// in-flight requests rather than just submission.
    Future<LLMResponse> complete(const Json& messages,
                                 const Json& tools = Json()) override {
        // Block until rate limit allows.
        rate_.acquire(1);

        // Block until a concurrency slot is available.
        sem_.acquire();

        // Delegate to inner client. Release semaphore on exception.
        Future<LLMResponse> inner_fut;
        try {
            inner_fut = inner_.complete(messages, tools);
        } catch (...) {
            sem_.release();
            throw;
        }

        // Chain: release the semaphore when the inner future resolves.
        // This avoids occupying a thread pool thread for waiting.
        auto promise = std::make_shared<Promise<LLMResponse>>();
        auto result_fut = promise->get_future();

        auto* sem_ptr = &sem_;
        inner_fut.then(
            [sem_ptr, p = std::move(promise)](LLMResponse resp) {
                sem_ptr->release();
                p->set_value(std::move(resp));
            });

        return result_fut;
    }

    const LLMClientConfig& config() const override {
        return inner_.config();
    }

private:
    ILLMClient& inner_;
    Semaphore& sem_;
    RateLimiter& rate_;
};

}  // namespace forge
