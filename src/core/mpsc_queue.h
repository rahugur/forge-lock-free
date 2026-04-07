#pragma once
// Lock-free Multi-Producer, Single-Consumer queue.
// Based on Dmitry Vyukov's intrusive MPSC queue algorithm.
// Multiple threads can push concurrently; exactly one thread drains via pop().

#include <atomic>
#include <optional>
#include <utility>

namespace forge {

template <typename T>
class MPSCQueue {
public:
    MPSCQueue() {
        auto* sentinel = new Node{};
        head_.store(sentinel, std::memory_order_relaxed);
        tail_ = sentinel;
    }

    ~MPSCQueue() {
        // Drain remaining nodes
        while (pop().has_value()) {}
        delete tail_;  // delete sentinel
    }

    // Non-copyable, non-movable
    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    /// Push a value (thread-safe, lock-free for multiple producers).
    void push(T value) {
        auto* node = new Node(std::move(value));
        // Atomically swap head to point to new node. 
        // Previous head becomes our predecessor.
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        // Link predecessor to us. This linearizes the push.
        prev->next.store(node, std::memory_order_release);
    }

    /// Pop a value (single consumer only — NOT thread-safe for multiple consumers).
    /// Returns std::nullopt if queue is empty.
    std::optional<T> pop() {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return std::nullopt;
        }
        // Advance tail past the sentinel to 'next'
        tail_ = next;
        std::optional<T> value = std::move(next->value);
        delete tail;  // delete old sentinel
        return value;
    }

    /// Check if the queue appears empty (approximate — may race with push).
    bool empty() const {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    struct Node {
        std::atomic<Node*> next{nullptr};
        std::optional<T> value;

        Node() = default;
        explicit Node(T v) : value(std::move(v)) {}
    };

    // Producers CAS on head_
    alignas(64) std::atomic<Node*> head_;
    // Consumer reads from tail_ (no contention — single consumer)
    alignas(64) Node* tail_;
};

}  // namespace forge
