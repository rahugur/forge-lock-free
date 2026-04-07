#pragma once
// Chase-Lev work-stealing deque.
// Owner thread pushes/pops from the bottom (LIFO — cache-friendly).
// Thief threads steal from the top (FIFO — coarse-grained work first).
// 
// Reference: "Dynamic Circular Work-Stealing Deque" — Chase & Lev, SPAA 2005.

#include <atomic>
#include <optional>
#include <vector>
#include <cstdint>
#include <cassert>
#include <memory>

namespace forge {

template <typename T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(int64_t capacity = 1024)
        : top_(0), bottom_(0) {
        buffer_.store(new CircularBuffer(capacity), std::memory_order_relaxed);
    }

    ~WorkStealingDeque() {
        delete buffer_.load(std::memory_order_relaxed);
        // Clean up old buffers
        for (auto* buf : old_buffers_) {
            delete buf;
        }
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /// Push to the bottom (owner thread only).
    void push(T value) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        auto* buf = buffer_.load(std::memory_order_relaxed);

        if (b - t >= buf->capacity()) {
            // Grow the buffer
            auto* new_buf = buf->grow(t, b);
            old_buffers_.push_back(buf);
            buffer_.store(new_buf, std::memory_order_release);
            buf = new_buf;
        }

        buf->put(b, std::move(value));
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    /// Pop from the bottom (owner thread only). Returns nullopt if empty.
    std::optional<T> pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        auto* buf = buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64_t t = top_.load(std::memory_order_relaxed);
        if (t <= b) {
            // Non-empty
            auto value = buf->get(b);
            if (t == b) {
                // Last element — race with steal
                int64_t expected_t = t;
                if (!top_.compare_exchange_strong(expected_t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    // Lost race to a thief.
                    // Note: expected_t is modified by CAS on failure!
                    // Restore bottom_ to t + 1 (the new empty state where top == t + 1).
                    bottom_.store(t + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom_.store(t + 1, std::memory_order_relaxed);
            }
            return std::move(value);
        } else {
            // Empty
            bottom_.store(t, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    /// Steal from the top (thief threads — thread-safe).
    std::optional<T> steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        if (t < b) {
            auto* buf = buffer_.load(std::memory_order_consume);
            auto value = buf->get(t);
            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                // Another thief got it
                return std::nullopt;
            }
            return std::move(value);
        }
        return std::nullopt;
    }

    /// Approximate size (may race).
    int64_t size() const {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return std::max(b - t, int64_t(0));
    }

    bool empty() const { return size() == 0; }

private:
    struct CircularBuffer {
        explicit CircularBuffer(int64_t cap) : cap_(cap), data_(new std::atomic<T>[cap]) {
            for (int64_t i = 0; i < cap; ++i) {
                std::atomic_init(&data_[i], T{});
            }
        }

        int64_t capacity() const { return cap_; }

        void put(int64_t idx, T value) {
            data_[idx % cap_].store(value, std::memory_order_relaxed);
        }

        T get(int64_t idx) const {
            return data_[idx % cap_].load(std::memory_order_relaxed);
        }

        CircularBuffer* grow(int64_t top, int64_t bottom) {
            auto* new_buf = new CircularBuffer(cap_ * 2);
            for (int64_t i = top; i < bottom; ++i) {
                new_buf->put(i, get(i));
            }
            return new_buf;
        }

    private:
        int64_t cap_;
        std::unique_ptr<std::atomic<T>[]> data_;
    };

    alignas(64) std::atomic<int64_t> top_;
    alignas(64) std::atomic<int64_t> bottom_;
    std::atomic<CircularBuffer*> buffer_;
    std::vector<CircularBuffer*> old_buffers_;  // prevent premature dealloc
};

}  // namespace forge
