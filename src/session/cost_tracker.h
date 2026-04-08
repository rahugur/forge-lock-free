#pragma once
// Per-session cost tracking.
// Atomic token counters + model pricing lookup.

#include "utils/json.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace forge {

struct ModelPricing {
    double prompt_cost_per_1k = 0.0;
    double completion_cost_per_1k = 0.0;
};

class CostTracker {
public:
    void set_pricing(const std::string& model, ModelPricing pricing) {
        std::lock_guard lock(mu_);
        pricing_[model] = pricing;
    }

    void record(const std::string& model, int prompt_tokens, int completion_tokens) {
        prompt_tokens_.fetch_add(prompt_tokens, std::memory_order_relaxed);
        completion_tokens_.fetch_add(completion_tokens, std::memory_order_relaxed);

        std::lock_guard lock(mu_);
        auto& t = per_model_tokens_[model];
        t.first += prompt_tokens;
        t.second += completion_tokens;
    }

    int total_prompt_tokens() const {
        return prompt_tokens_.load(std::memory_order_relaxed);
    }

    int total_completion_tokens() const {
        return completion_tokens_.load(std::memory_order_relaxed);
    }

    double total_cost() const {
        std::lock_guard lock(mu_);
        double cost = 0.0;
        for (auto& [model, tokens] : per_model_tokens_) {
            auto pit = pricing_.find(model);
            if (pit != pricing_.end()) {
                cost += (tokens.first / 1000.0) * pit->second.prompt_cost_per_1k;
                cost += (tokens.second / 1000.0) * pit->second.completion_cost_per_1k;
            }
        }
        return cost;
    }

    Json to_json() const {
        std::lock_guard lock(mu_);
        Json j;
        j["prompt_tokens"] = prompt_tokens_.load(std::memory_order_relaxed);
        j["completion_tokens"] = completion_tokens_.load(std::memory_order_relaxed);
        j["total_cost"] = total_cost_locked();
        return j;
    }

private:
    double total_cost_locked() const {
        double cost = 0.0;
        for (auto& [model, tokens] : per_model_tokens_) {
            auto pit = pricing_.find(model);
            if (pit != pricing_.end()) {
                cost += (tokens.first / 1000.0) * pit->second.prompt_cost_per_1k;
                cost += (tokens.second / 1000.0) * pit->second.completion_cost_per_1k;
            }
        }
        return cost;
    }

    std::atomic<int> prompt_tokens_{0};
    std::atomic<int> completion_tokens_{0};

    mutable std::mutex mu_;
    std::unordered_map<std::string, ModelPricing> pricing_;
    std::unordered_map<std::string, std::pair<int, int>> per_model_tokens_;
};

}  // namespace forge
