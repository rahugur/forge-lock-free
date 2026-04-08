#pragma once
// Lock-free Future/Promise for the Forge runtime.
//
// set_value(), set_exception(), and then() are lock-free (no mutex, no condvar).
// get() and wait_for() spin with exponential backoff — blocking by nature
// but never hold any lock.
//
// Preconditions:
//   - Call either get() OR then(), not both.
//   - Call then() at most once per future.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <cassert>

namespace forge {

template <typename T> class Future;
template <typename T> class Promise;

namespace detail {

// ── Yield callback ──────────────────────────────────────────────
// ThreadPool sets this on worker threads so that Future::get() can
// process pool tasks while waiting instead of sleeping.
// Returns true if a task was processed.
inline thread_local bool (*yield_fn)() = nullptr;

// ── Backoff for spin-wait ────────────────────────────────────────

inline void backoff(unsigned iter) {
    // If we're on a pool thread, try to process a task instead of sleeping.
    if (yield_fn && yield_fn()) {
        return;  // did useful work
    }
    if (iter < 4) {
        // Busy-spin — value expected imminently.
    } else if (iter < 16) {
        std::this_thread::yield();
    } else if (iter < 64) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// ── Sentinel pointer ─────────────────────────────────────────────
// Marks the continuation slot as "consumed by set_value / set_exception".
// Any non-null, non-dereferenceable address works.

template <typename F>
F* sentinel() {
    return reinterpret_cast<F*>(std::uintptr_t(1));
}

template <typename F>
bool is_sentinel(F* p) {
    return p == sentinel<F>();
}

// ── SharedState<T> ──────────────────────────────────────────────
//
// Layout:
//   value_storage_ — placement-new'd at most once (before ready_ is set).
//   exception_     — written at most once (before ready_ is set).
//   ready_         — published with release; consumed with acquire.
//   continuation_  — lock-free exchange between set_value and then().
//
// Protocol:
//   set_value:  construct value → ready_.store(true, release) →
//               continuation_.exchange(sentinel, acq_rel);
//               if prev was a real pointer, invoke & delete it.
//
//   then:       allocate Cont* → continuation_.exchange(cont, acq_rel);
//               if prev was sentinel, value is ready — invoke & delete cont.

template <typename T>
struct SharedState {
    alignas(T) unsigned char value_storage_[sizeof(T)];
    std::exception_ptr exception_;

    std::atomic<bool> ready_{false};

    using Cont = std::function<void(T)>;
    std::atomic<Cont*> continuation_{nullptr};

    T*       value_ptr()       { return reinterpret_cast<T*>(value_storage_); }
    const T* value_ptr() const { return reinterpret_cast<const T*>(value_storage_); }

    ~SharedState() {
        if (ready_.load(std::memory_order_relaxed) && !exception_) {
            value_ptr()->~T();
        }
        auto* c = continuation_.load(std::memory_order_relaxed);
        if (c && !is_sentinel(c)) {
            delete c;
        }
    }
};

template <>
struct SharedState<void> {
    std::exception_ptr exception_;
    std::atomic<bool> ready_{false};

    using Cont = std::function<void()>;
    std::atomic<Cont*> continuation_{nullptr};

    ~SharedState() {
        auto* c = continuation_.load(std::memory_order_relaxed);
        if (c && !is_sentinel(c)) {
            delete c;
        }
    }
};

}  // namespace detail

// ═════════════════════════════════════════════════════════════════
//  Promise<T>
// ═════════════════════════════════════════════════════════════════

template <typename T>
class Promise {
public:
    Promise() : state_(std::make_shared<detail::SharedState<T>>()) {}

    Future<T> get_future() { return Future<T>(state_); }

    /// Lock-free.  Must be called at most once.
    void set_value(T value) {
        auto& s = *state_;
        assert(!s.ready_.load(std::memory_order_relaxed)
               && "Promise already satisfied");

        new (s.value_ptr()) T(std::move(value));
        s.ready_.store(true, std::memory_order_release);

        using Cont = typename detail::SharedState<T>::Cont;
        auto* prev = s.continuation_.exchange(
            detail::sentinel<Cont>(), std::memory_order_acq_rel);

        if (prev && !detail::is_sentinel(prev)) {
            (*prev)(*s.value_ptr());
            delete prev;
        }
    }

    /// Lock-free.  Must be called at most once.
    void set_exception(std::exception_ptr ep) {
        auto& s = *state_;
        assert(!s.ready_.load(std::memory_order_relaxed)
               && "Promise already satisfied");

        s.exception_ = ep;
        s.ready_.store(true, std::memory_order_release);

        using Cont = typename detail::SharedState<T>::Cont;
        auto* prev = s.continuation_.exchange(
            detail::sentinel<Cont>(), std::memory_order_acq_rel);

        if (prev && !detail::is_sentinel(prev)) {
            delete prev;  // no value to invoke with
        }
    }

private:
    std::shared_ptr<detail::SharedState<T>> state_;
};

// ═════════════════════════════════════════════════════════════════
//  Future<T>
// ═════════════════════════════════════════════════════════════════

template <typename T>
class Future {
public:
    Future() = default;

    /// Spin-wait until ready, return the value or rethrow.
    T get() {
        assert(state_ && "Future not valid");
        unsigned iter = 0;
        while (!state_->ready_.load(std::memory_order_acquire)) {
            detail::backoff(iter++);
        }
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        return std::move(*state_->value_ptr());
    }

    /// Timed wait.  Returns std::nullopt on timeout.
    template <typename Rep, typename Period>
    std::optional<T> wait_for(const std::chrono::duration<Rep, Period>& dur) {
        assert(state_ && "Future not valid");
        auto deadline = std::chrono::steady_clock::now() + dur;
        unsigned iter = 0;
        while (!state_->ready_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            detail::backoff(iter++);
        }
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        return std::move(*state_->value_ptr());
    }

    /// Lock-free continuation.  Fires exactly once when the value arrives.
    /// Must be called at most once.  Do not also call get().
    void then(std::function<void(T)> callback) {
        assert(state_ && "Future not valid");
        using Cont = typename detail::SharedState<T>::Cont;
        auto* cont = new Cont(std::move(callback));

        auto* prev = state_->continuation_.exchange(
            cont, std::memory_order_acq_rel);

        if (detail::is_sentinel(prev)) {
            // set_value already ran — invoke ourselves, outside any lock.
            if (!state_->exception_) {
                (*cont)(*state_->value_ptr());
            }
            delete cont;
            // Mark as consumed so ~SharedState doesn't double-free.
            state_->continuation_.store(
                detail::sentinel<Cont>(), std::memory_order_relaxed);
        }
        // else: stored; set_value/set_exception will pick it up.
    }

    bool ready() const {
        return state_ && state_->ready_.load(std::memory_order_acquire);
    }

    bool valid() const { return state_ != nullptr; }

private:
    friend class Promise<T>;
    explicit Future(std::shared_ptr<detail::SharedState<T>> st)
        : state_(std::move(st)) {}

    std::shared_ptr<detail::SharedState<T>> state_;
};

// ═════════════════════════════════════════════════════════════════
//  Promise<void> / Future<void>
// ═════════════════════════════════════════════════════════════════

template <>
class Promise<void> {
public:
    Promise() : state_(std::make_shared<detail::SharedState<void>>()) {}

    Future<void> get_future();

    void set_value() {
        auto& s = *state_;
        assert(!s.ready_.load(std::memory_order_relaxed)
               && "Promise already satisfied");

        s.ready_.store(true, std::memory_order_release);

        using Cont = detail::SharedState<void>::Cont;
        auto* prev = s.continuation_.exchange(
            detail::sentinel<Cont>(), std::memory_order_acq_rel);

        if (prev && !detail::is_sentinel(prev)) {
            (*prev)();
            delete prev;
        }
    }

    void set_exception(std::exception_ptr ep) {
        auto& s = *state_;
        assert(!s.ready_.load(std::memory_order_relaxed)
               && "Promise already satisfied");

        s.exception_ = ep;
        s.ready_.store(true, std::memory_order_release);

        using Cont = detail::SharedState<void>::Cont;
        auto* prev = s.continuation_.exchange(
            detail::sentinel<Cont>(), std::memory_order_acq_rel);

        if (prev && !detail::is_sentinel(prev)) {
            delete prev;
        }
    }

private:
    std::shared_ptr<detail::SharedState<void>> state_;
};

template <>
class Future<void> {
public:
    Future() = default;

    void get() {
        assert(state_ && "Future not valid");
        unsigned iter = 0;
        while (!state_->ready_.load(std::memory_order_acquire)) {
            detail::backoff(iter++);
        }
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
    }

    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& dur) {
        assert(state_ && "Future not valid");
        auto deadline = std::chrono::steady_clock::now() + dur;
        unsigned iter = 0;
        while (!state_->ready_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            detail::backoff(iter++);
        }
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        return true;
    }

    void then(std::function<void()> callback) {
        assert(state_ && "Future not valid");
        using Cont = detail::SharedState<void>::Cont;
        auto* cont = new Cont(std::move(callback));

        auto* prev = state_->continuation_.exchange(
            cont, std::memory_order_acq_rel);

        if (detail::is_sentinel(prev)) {
            if (!state_->exception_) {
                (*cont)();
            }
            delete cont;
            // Mark as consumed so ~SharedState doesn't double-free.
            state_->continuation_.store(
                detail::sentinel<Cont>(), std::memory_order_relaxed);
        }
    }

    bool ready() const {
        return state_ && state_->ready_.load(std::memory_order_acquire);
    }

    bool valid() const { return state_ != nullptr; }

private:
    friend class Promise<void>;
    explicit Future(std::shared_ptr<detail::SharedState<void>> st)
        : state_(std::move(st)) {}

    std::shared_ptr<detail::SharedState<void>> state_;
};

inline Future<void> Promise<void>::get_future() {
    return Future<void>(state_);
}

}  // namespace forge
