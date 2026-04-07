#pragma once
// Lightweight Future/Promise pair for the Forge runtime.
// Not lock-free (uses mutex+condvar) but futures are consumed once,
// so contention is minimal.

#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <exception>
#include <chrono>
#include <cassert>

namespace forge {

template <typename T>
class Future;

template <typename T>
class Promise;

namespace detail {

template <typename T>
struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<T> value;
    std::exception_ptr exception;
    bool ready = false;
    std::function<void(T)> continuation;
};

}  // namespace detail

template <>
struct detail::SharedState<void> {
    std::mutex mutex;
    std::condition_variable cv;
    std::exception_ptr exception;
    bool ready = false;
    std::function<void()> continuation;
};

template <typename T>
class Promise {
public:
    Promise() : state_(std::make_shared<detail::SharedState<T>>()) {}

    Future<T> get_future() {
        return Future<T>(state_);
    }

    void set_value(T value) {
        std::function<void(T)> cont;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->ready) {
                throw std::logic_error("Promise already satisfied");
            }
            state_->value = std::move(value);
            state_->ready = true;
            cont = std::move(state_->continuation);
        }
        state_->cv.notify_all();
        if (cont) {
            cont(std::move(*state_->value));
        }
    }

    void set_exception(std::exception_ptr ep) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->ready) {
                throw std::logic_error("Promise already satisfied");
            }
            state_->exception = ep;
            state_->ready = true;
        }
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<detail::SharedState<T>> state_;
};

template <typename T>
class Future {
public:
    Future() = default;

    /// Blocking wait — returns the value or rethrows the stored exception.
    T get() {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait(lock, [this] { return state_->ready; });
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->value);
    }

    /// Timed wait — returns nullopt on timeout.
    template <typename Rep, typename Period>
    std::optional<T> wait_for(const std::chrono::duration<Rep, Period>& dur) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->cv.wait_for(lock, dur, [this] { return state_->ready; })) {
            return std::nullopt;
        }
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->value);
    }

    /// Non-blocking continuation: callback fires when value is ready.
    void then(std::function<void(T)> callback) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->ready) {
            if (state_->value) {
                callback(std::move(*state_->value));
            }
        } else {
            state_->continuation = std::move(callback);
        }
    }

    bool ready() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->ready;
    }

    bool valid() const { return state_ != nullptr; }

private:
    friend class Promise<T>;
    explicit Future(std::shared_ptr<detail::SharedState<T>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::SharedState<T>> state_;
};

template <>
class Promise<void> {
public:
    Promise() : state_(std::make_shared<detail::SharedState<void>>()) {}

    Future<void> get_future();

    void set_value() {
        std::function<void()> cont;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->ready) {
                throw std::logic_error("Promise already satisfied");
            }
            state_->ready = true;
            cont = std::move(state_->continuation);
        }
        state_->cv.notify_all();
        if (cont) {
            cont();
        }
    }

    void set_exception(std::exception_ptr ep) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->ready) {
                throw std::logic_error("Promise already satisfied");
            }
            state_->exception = ep;
            state_->ready = true;
        }
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<detail::SharedState<void>> state_;
};

template <>
class Future<void> {
public:
    Future() = default;

    void get() {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait(lock, [this] { return state_->ready; });
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
    }

    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& dur) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->cv.wait_for(lock, dur, [this] { return state_->ready; })) {
            return false;
        }
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return true;
    }

    void then(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->ready) {
            if (!state_->exception) {
                callback();
            }
        } else {
            state_->continuation = std::move(callback);
        }
    }

    bool ready() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->ready;
    }

    bool valid() const { return state_ != nullptr; }

private:
    friend class Promise<void>;
    explicit Future(std::shared_ptr<detail::SharedState<void>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::SharedState<void>> state_;
};

inline Future<void> Promise<void>::get_future() {
    return Future<void>(state_);
}

}  // namespace forge
