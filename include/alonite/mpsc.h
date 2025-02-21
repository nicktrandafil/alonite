#pragma once

#include "common.h"
#include "contract.h"
#include "runtime.h"

#include <list>
#include <memory>
#include <mutex>
#include <optional>

namespace alonite::mpsc {

struct ClosedError : std::exception {
    const char* what() const noexcept override {
        return "closed";
    }
};

namespace detail {

template <class T>
class UnboundState {
public:
    UnboundState() = default;
    UnboundState(UnboundState const&) = delete;
    UnboundState& operator=(UnboundState const&) = delete;

    /// \throw std::bad_alloc
    void push(T value) noexcept(false) {
        {
            std::scoped_lock lock{mutex};
            if (closed) {
                throw ClosedError{};
            } else {
                queue.push_back(std::move(value));
            }
        }
        cv.notify_one();
    }

    Task<std::optional<T>> pop() noexcept {
        std::unique_lock lock{mutex};
        while (true) {
            if (!queue.empty()) {
                auto ret = std::move(queue.front());
                queue.pop_front();
                co_return ret;
            }

            if (closed) {
                co_return std::nullopt;
            }

            lock.unlock();
            co_await cv.wait();
            lock.lock();
        }
    }

    void inc_sender() {
        std::unique_lock lock{mutex};
        sender_count += 1;
    }

    void dec_sender() {
        std::unique_lock lock{mutex};
        sender_count -= 1;
        if (sender_count == 0) {
            closed = true;
            lock.unlock();
            cv.notify_all();
        }
    }

    void close() {
        std::unique_lock lock{mutex};
        closed = true;
        lock.unlock();
        cv.notify_all();
    }

private:
    ConditionVariable cv;
    std::mutex mutex;
    std::deque<T> queue;
    bool closed = false;
    unsigned sender_count{1};
};

} // namespace detail

template <class T>
class UnboundSender;

/// \note The receiver is thread safe. You can share it among different threads.
template <class T>
class UnboundReceiver {
public:
    UnboundReceiver(UnboundReceiver const&) = delete;
    UnboundReceiver& operator=(UnboundReceiver const&) = delete;

    UnboundReceiver(UnboundReceiver&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    UnboundReceiver& operator=(UnboundReceiver&& rhs) noexcept {
        this->~UnboundReceiver();
        new (this) UnboundReceiver{std::move(rhs)};
        return *this;
    }

    ~UnboundReceiver() {
        if (state) {
            state.value()->close();
        }
    }

    Task<std::optional<T>> recv() noexcept {
        co_return co_await state.value()->pop();
    }

private:
    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(
            false);

    UnboundReceiver(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<T>>> state;
};

template <class T>
class UnboundSender {
public:
    UnboundSender(UnboundSender const& rhs)
            : state{rhs.state} {
        if (state) {
            state.value()->inc_sender();
        }
    }

    UnboundSender& operator=(UnboundSender const& rhs) {
        UnboundSender tmp{rhs};
        return *this = std::move(tmp);
    }

    UnboundSender(UnboundSender&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    UnboundSender& operator=(UnboundSender&& rhs) noexcept {
        this->~UnboundSender();
        new (this) UnboundSender{std::move(rhs)};
        return *this;
    }

    ~UnboundSender() {
        if (state) {
            state.value()->dec_sender();
        }
    }

    /// \throw std::bad_alloc, ClosedError
    void send(T value) const noexcept(false) {
        state.value()->push(std::move(value));
    }

private:
    UnboundSender() = default;

    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(
            false);

    UnboundSender(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<T>>> state;
};

/// \throw std::bad_alloc
template <class T>
std::pair<UnboundSender<T>, UnboundReceiver<T>> unbound_channel() noexcept(false) {
    auto state = std::make_shared<detail::UnboundState<T>>();
    return {UnboundSender<T>{state}, UnboundReceiver<T>{std::move(state)}};
}

} // namespace alonite::mpsc
