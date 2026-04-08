#pragma once
// Bounded ring-buffer conversation history.
// When capacity is reached, the oldest non-system messages are evicted.
// System messages (index 0) are always preserved.

#include "llm/message.h"

#include <deque>
#include <string>
#include <vector>

namespace forge {

class Conversation {
public:
    explicit Conversation(size_t max_messages = 100)
        : max_messages_(max_messages) {}

    /// Append a message to the history.
    void push(Message msg) {
        messages_.push_back(std::move(msg));
        evict_if_needed();
    }

    /// Access the most recent message.  UB if empty.
    const Message& back() const { return messages_.back(); }

    /// Number of messages in the history.
    size_t size() const { return messages_.size(); }

    bool empty() const { return messages_.empty(); }

    /// Read-only view of all messages.
    const std::deque<Message>& messages() const { return messages_; }

    /// Serialize all messages to a JSON array (for LLM request).
    Json to_json() const {
        Json arr = Json::array();
        for (auto& m : messages_) {
            arr.push_back(m.to_json());
        }
        return arr;
    }

    /// Clear all messages.
    void clear() { messages_.clear(); }

    /// Maximum capacity.
    size_t capacity() const { return max_messages_; }

private:
    void evict_if_needed() {
        while (messages_.size() > max_messages_) {
            // Preserve the system message at index 0 if present.
            if (messages_.size() > 1 && messages_.front().role == "system") {
                // Remove the second message (oldest non-system).
                messages_.erase(messages_.begin() + 1);
            } else {
                messages_.pop_front();
            }
        }
    }

    size_t max_messages_;
    std::deque<Message> messages_;
};

}  // namespace forge
