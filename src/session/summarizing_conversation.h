#pragma once
// SummarizingConversation: wraps Conversation with LLM-based summarization.
// When message count exceeds a threshold, older messages are summarized
// into a single message, preserving the system message and recent messages.

#include "llm/llm_client_interface.h"
#include "llm/message.h"
#include "session/conversation.h"

#include <spdlog/spdlog.h>

#include <string>

namespace forge {

class SummarizingConversation {
public:
    SummarizingConversation(ILLMClient& llm, size_t max_messages = 100,
                            size_t summarize_threshold = 80,
                            size_t keep_recent = 10)
        : llm_(llm), conversation_(max_messages),
          summarize_threshold_(summarize_threshold),
          keep_recent_(keep_recent) {}

    void push(Message msg) {
        conversation_.push(std::move(msg));
        maybe_summarize();
    }

    const Message& back() const { return conversation_.back(); }
    size_t size() const { return conversation_.size(); }
    bool empty() const { return conversation_.empty(); }
    const std::deque<Message>& messages() const { return conversation_.messages(); }
    Json to_json() const { return conversation_.to_json(); }
    void clear() { conversation_.clear(); }
    size_t capacity() const { return conversation_.capacity(); }

private:
    void maybe_summarize() {
        if (conversation_.size() < summarize_threshold_) return;

        auto& msgs = conversation_.messages();

        // Determine which messages to summarize (skip system + keep recent).
        size_t start = 0;
        if (!msgs.empty() && msgs[0].role == "system") {
            start = 1;
        }

        size_t end = msgs.size() > keep_recent_ ? msgs.size() - keep_recent_ : start;
        if (end <= start + 1) return;  // nothing worth summarizing

        // Build messages to summarize.
        Json to_summarize = Json::array();
        for (size_t i = start; i < end; ++i) {
            to_summarize.push_back(msgs[i].to_json());
        }

        // Ask LLM to summarize.
        Json request = Json::array();
        Message sys;
        sys.role = "system";
        sys.content = "Summarize the following conversation concisely, "
                      "preserving all key information, decisions, and results. "
                      "Respond with a single summary paragraph.";
        request.push_back(sys.to_json());

        Message user;
        user.role = "user";
        user.content = "Conversation to summarize:\n" + to_summarize.dump(2);
        request.push_back(user.to_json());

        auto future = llm_.complete(request);
        auto response = future.get();

        if (!response.error.empty() || response.message.content.empty()) {
            spdlog::warn("[summarize] LLM summarization failed, falling back to eviction");
            return;  // let normal eviction handle it
        }

        // Rebuild conversation: system + summary + recent messages.
        Conversation new_conv(conversation_.capacity());

        if (!msgs.empty() && msgs[0].role == "system") {
            new_conv.push(msgs[0]);
        }

        Message summary;
        summary.role = "assistant";
        summary.content = "[Summary of prior conversation]\n" + response.message.content;
        new_conv.push(std::move(summary));

        for (size_t i = end; i < msgs.size(); ++i) {
            new_conv.push(msgs[i]);
        }

        conversation_ = std::move(new_conv);

        spdlog::info("[summarize] Compressed {} messages to {} (summary + {} recent)",
                     end - start, conversation_.size(), keep_recent_);
    }

    ILLMClient& llm_;
    Conversation conversation_;
    size_t summarize_threshold_;
    size_t keep_recent_;
};

}  // namespace forge
