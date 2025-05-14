#pragma once

#include "closed_error.h"
#include "common.h"
#include "runtime.h"

#include <memory>
#include <mutex>
#include <optional>

namespace alonite::mpsc {
namespace detail {

template <class T>
struct CommonState {
    ConditionVariable consumer_cv;
    std::mutex mutex;
    std::deque<T> queue;
    bool closed = false;
    unsigned sender_count{1};

    void inc_sender() {
        std::scoped_lock lock{mutex};
        sender_count += 1;
    }

    void dec_sender() {
        std::unique_lock lock{mutex};
        sender_count -= 1;
        if (sender_count == 0) {
            closed = true;
            lock.unlock();
            consumer_cv.notify_one();
        }
    }

    unsigned get_sender_count() {
        std::scoped_lock lock{mutex};
        return sender_count;
    }

    bool is_receiver_closed() {
        std::scoped_lock lock{mutex};
        return closed;
    }
};

template <>
struct CommonState<void> {
    ConditionVariable consumer_cv;
    std::mutex mutex;
    uint64_t queue{0};
    bool closed = false;
    unsigned sender_count{1};

    void inc_sender() {
        std::scoped_lock lock{mutex};
        sender_count += 1;
    }

    void dec_sender() {
        std::unique_lock lock{mutex};
        sender_count -= 1;
        if (sender_count == 0) {
            closed = true;
            lock.unlock();
            consumer_cv.notify_one();
        }
    }

    unsigned get_sender_count() {
        std::scoped_lock lock{mutex};
        return sender_count;
    }

    bool is_receiver_closed() {
        std::scoped_lock lock{mutex};
        return closed;
    }
};

template <class T>
class UnboundState : public CommonState<T> {
public:
    UnboundState() = default;

    UnboundState(UnboundState const&) = delete;

    UnboundState& operator=(UnboundState const&) = delete;

    /// \throw std::bad_alloc, ClosedError
    /// \post Moved only and only if succeeded.
    template <class U>
        requires(std::is_same_v<T, std::decay_t<U>>)
    void push(U&& value) noexcept(false) {
        {
            std::scoped_lock lock{this->mutex};
            if (this->closed) {
                throw ClosedError{};
            } else {
                this->queue.push_back(std::forward<U>(value));
            }
        }
        this->consumer_cv.notify_one();
    }

    Task<std::optional<T>> pop() noexcept {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (!this->queue.empty()) {
                auto ret = std::move(this->queue.front());
                this->queue.pop_front();
                co_return ret;
            }

            if (this->closed) {
                co_return std::nullopt;
            }

            co_await this->consumer_cv.wait(lock);
        }
    }

    void dec_receiver() {
        std::unique_lock lock{this->mutex};
        this->closed = true;
        lock.unlock();
    }
};

template <>
class UnboundState<void> : public CommonState<void> {
public:
    UnboundState() = default;

    UnboundState(UnboundState const&) = delete;

    UnboundState& operator=(UnboundState const&) = delete;

    /// \throw std::bad_alloc, ClosedError
    void push() noexcept(false) {
        {
            std::scoped_lock lock{this->mutex};
            if (this->closed) {
                throw ClosedError{};
            } else {
                ++queue;
            }
        }
        this->consumer_cv.notify_one();
    }

    Task<bool> pop() noexcept {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->queue) {
                --this->queue;
                co_return true;
            }

            if (this->closed) {
                co_return false;
            }

            co_await this->consumer_cv.wait(lock);
        }
    }

    void dec_receiver() {
        std::unique_lock lock{this->mutex};
        this->closed = true;
        lock.unlock();
    }
};

template <class T>
class State : public CommonState<T> {
public:
    explicit State(size_t limit)
            : limit{limit} {};

    State(State const&) = delete;

    State& operator=(State const&) = delete;

    /// \throw std::bad_alloc, ClosedError
    Task<void> push(T value) noexcept(false) {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->closed) {
                throw ClosedError{std::move(value)};
            } else if (this->queue.size() < limit) {
                this->queue.push_back(std::move(value));
                break;
            } else {
                co_await producer_cv.wait(lock);
            }
        }
        lock.unlock();
        this->consumer_cv.notify_one();
    }

    template <std::convertible_to<T> U>
    bool try_push(U&& value) noexcept(false) {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->closed) {
                throw ClosedError{static_cast<T>(std::forward<U>(value))};
            } else if (this->queue.size() < limit) {
                this->queue.push_back(std::forward<U>(value));
                break;
            } else {
                return false;
            }
        }
        lock.unlock();
        this->consumer_cv.notify_one();
        return true;
    }

    Task<std::optional<T>> pop() noexcept {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (!this->queue.empty()) {
                auto ret = std::move(this->queue.front());
                this->queue.pop_front();
                lock.unlock();
                producer_cv.notify_one();
                co_return ret;
            }

            if (this->closed) {
                co_return std::nullopt;
            }

            co_await this->consumer_cv.wait(lock);
        }
    }

    void dec_receiver() {
        std::unique_lock lock{this->mutex};
        this->closed = true;
        lock.unlock();
        producer_cv.notify_all();
    }

private:
    size_t const limit;
    ConditionVariable producer_cv;
};

template <>
class State<void> : public CommonState<void> {
public:
    explicit State(size_t limit)
            : limit{limit} {};

    State(State const&) = delete;

    State& operator=(State const&) = delete;

    Task<void> push() noexcept(false) {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->closed) {
                throw ClosedError{};
            } else if (this->queue < limit) {
                ++this->queue;
                break;
            } else {
                co_await producer_cv.wait(lock);
            }
        }
        lock.unlock();
        this->consumer_cv.notify_one();
    }

    bool try_push() noexcept(false) {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->closed) {
                throw ClosedError{};
            } else if (this->queue < limit) {
                ++this->queue;
                break;
            } else {
                return false;
            }
        }
        lock.unlock();
        this->consumer_cv.notify_one();
        return true;
    }

    Task<bool> pop() noexcept {
        std::unique_lock lock{this->mutex};
        while (true) {
            if (this->queue) {
                --this->queue;
                lock.unlock();
                producer_cv.notify_one();
                co_return true;
            }

            if (this->closed) {
                co_return false;
            }

            co_await this->consumer_cv.wait(lock);
        }
    }

    void dec_receiver() {
        std::unique_lock lock{this->mutex};
        this->closed = true;
        lock.unlock();
        producer_cv.notify_all();
    }

private:
    size_t const limit;
    ConditionVariable producer_cv;
};

} // namespace detail

template <class T>
class UnboundSender;

/// \note The receiver is thread safe. You can share it among different threads,
/// but the next value will received only by on thread.
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
            state.value()->dec_receiver();
        }
    }

    Task<std::optional<T>> recv() noexcept {
        co_return co_await state.value()->pop();
    }

private:
    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(false);

    UnboundReceiver(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<T>>> state;
};

/// \note The receiver is thread safe. You can share it among different threads,
/// but the next value will received only by on thread.
template <>
class UnboundReceiver<void> {
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
            state.value()->dec_receiver();
        }
    }

    Task<bool> recv() noexcept {
        co_return co_await state.value()->pop();
    }

private:
    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(false);

    UnboundReceiver(std::shared_ptr<detail::UnboundState<void>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<void>>> state;
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
    /// \post Moved only on success
    void send(T&& value) const noexcept(false) {
        state.value()->push(std::move(value));
    }

    /// \throw std::bad_alloc, ClosedError
    /// \post copied only on success
    void send(T const& value) const noexcept(false) {
        state.value()->push(value);
    }

private:
    UnboundSender() = default;

    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(false);

    UnboundSender(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<T>>> state;
};

template <>
class UnboundSender<void> {
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
    void send() const noexcept(false) {
        state.value()->push();
    }

private:
    UnboundSender() = default;

    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(false);

    UnboundSender(std::shared_ptr<detail::UnboundState<void>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::UnboundState<void>>> state;
};

template <class T>
class Sender;

/// \note The receiver is thread safe. You can share it among different threads,
/// but the next value will received only by on thread.
template <class T>
class Receiver {
public:
    Receiver(Receiver const&) = delete;
    Receiver& operator=(Receiver const&) = delete;

    Receiver(Receiver&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    Receiver& operator=(Receiver&& rhs) noexcept {
        this->~Receiver();
        new (this) Receiver{std::move(rhs)};
        return *this;
    }

    ~Receiver() {
        if (state) {
            state.value()->dec_receiver();
        }
    }

    /// \throw ClosedError
    Task<std::optional<T>> recv() noexcept {
        co_return co_await state.value()->pop();
    }

    bool is_closed() const {
        return state.value()->get_sender_count() == 0;
    }

private:
    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> channel(size_t limit) noexcept(false);

    Receiver(std::shared_ptr<detail::State<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::State<T>>> state;
};

template <>
class Receiver<void> {
public:
    Receiver(Receiver const&) = delete;

    Receiver& operator=(Receiver const&) = delete;

    Receiver(Receiver&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    Receiver& operator=(Receiver&& rhs) noexcept {
        this->~Receiver();
        new (this) Receiver{std::move(rhs)};
        return *this;
    }

    ~Receiver() {
        if (state) {
            state.value()->dec_receiver();
        }
    }

    /// \throw ClosedError
    Task<bool> recv() noexcept {
        co_return co_await state.value()->pop();
    }

    bool is_closed() const {
        return state.value()->get_sender_count() == 0;
    }

private:
    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> channel(size_t limit) noexcept(false);

    Receiver(std::shared_ptr<detail::State<void>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::State<void>>> state;
};

template <class T>
class Sender {
public:
    Sender(Sender const& rhs)
            : state{rhs.state} {
        if (state) {
            (*state)->inc_sender();
        }
    }

    Sender& operator=(Sender const& rhs) {
        Sender tmp{rhs};
        return *this = std::move(tmp);
    }

    Sender(Sender&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    Sender& operator=(Sender&& rhs) noexcept {
        this->~Sender();
        new (this) Sender{std::move(rhs)};
        return *this;
    }

    ~Sender() {
        if (state) {
            (*state)->dec_sender();
        }
    }

    /// \throw std::bad_alloc, ClosedError
    /// \note If the consumer is fast enough and the internal buffer
    /// doesn't fill up, then this call will not suspend. If you are in a loop,
    /// then you might want to `co_await Yield{}` to give up the thread for a moment.
    Task<void> send(T value) noexcept(false) {
        co_await state.value()->push(std::move(value));
    }

    template <std::convertible_to<T> U>
    bool try_send(U&& value) noexcept(false) {
        return state.value()->try_push(std::forward<U>(value));
    }

    bool is_closed() const {
        return state.value()->is_receiver_closed();
    }

private:
    Sender() = default;

    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> channel(size_t) noexcept(false);

    Sender(std::shared_ptr<detail::State<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::State<T>>> state;
};

template <>
class Sender<void> {
public:
    Sender(Sender const& rhs)
            : state{rhs.state} {
        if (state) {
            (*state)->inc_sender();
        }
    }

    Sender& operator=(Sender const& rhs) {
        Sender tmp{rhs};
        return *this = std::move(tmp);
    }

    Sender(Sender&& rhs) noexcept
            : state{take(rhs.state)} {
    }

    Sender& operator=(Sender&& rhs) noexcept {
        this->~Sender();
        new (this) Sender{std::move(rhs)};
        return *this;
    }

    ~Sender() {
        if (state) {
            (*state)->dec_sender();
        }
    }

    Task<void> send() noexcept(false) {
        co_await state.value()->push();
    }

    bool try_send() noexcept(false) {
        return state.value()->try_push();
    }

    bool is_closed() const {
        return state.value()->is_receiver_closed();
    }

private:
    Sender() = default;

    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> channel(size_t) noexcept(false);

    Sender(std::shared_ptr<detail::State<void>> state) noexcept
            : state{std::move(state)} {
    }

    std::optional<std::shared_ptr<detail::State<void>>> state;
};

/// \throw std::bad_alloc
template <class T>
std::pair<UnboundSender<T>, UnboundReceiver<T>> unbound_channel() noexcept(false) {
    auto state = std::make_shared<detail::UnboundState<T>>();
    return {UnboundSender<T>{state}, UnboundReceiver<T>{std::move(state)}};
}

/// \throw std::bad_alloc
template <class T>
std::pair<Sender<T>, Receiver<T>> channel(size_t limit) noexcept(false) {
    auto state = std::make_shared<detail::State<T>>(limit);
    return {Sender<T>{state}, Receiver<T>{std::move(state)}};
}

} // namespace alonite::mpsc
