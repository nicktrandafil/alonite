#pragma once

#include "closed_error.h"
#include "contract.h"
#include "runtime.h"
#include "scope_exit.h"

#include <deque>
#include <map>

namespace alonite::broadcast {

struct LaggedBehindError : std::exception {
    char const* what() const noexcept override {
        return "LaggedBehindError";
    }
};

struct NoReceiversError : std::exception {
    char const* what() const noexcept override {
        return "NoReceiversError";
    }
};

namespace detail {

template <class T>
struct State {
    explicit State(size_t limit)
            : limit{limit} {
        if (limit == 0) {
            throw std::invalid_argument{
                    "Broadcast channel buffer limit must be grater than 0"};
        }
    }

    struct Consumer {
        size_t index;
        bool lagged;
    };

    template <std::convertible_to<T> U>
    void send(U&& t) {
        std::unique_lock lock{mutex};
        ALONITE_SCOPE_EXIT {
            lock.unlock();
            cv.notify_all();
        };

        if (consumers.empty()) {
            throw NoReceiversError{};
        }

        if (queue.size() < limit) {
            queue.push_back(std::forward<U>(t));
            return;
        }

        // out of space, move the window, notify turtles
        auto const m =
                consumers_by_advancement.upper_bound(*consumers_by_advancement.begin());
        for (auto& x : std::ranges::subrange{consumers_by_advancement.begin(), m}) {
            consumers.at(x.second).lagged = true;
        }

        // adjust indexes to the window shift
        decrement_indexes(m, consumers_by_advancement.end());

        queue.pop_front();
        queue.push_back(std::forward<U>(t));
    }

    /// \throw LaggedBehindError
    Task<T> recv(size_t id) {
        std::unique_lock lock{mutex};

        for (;; lock.lock()) {
            if (queue.empty()) {
                if (senders_count) {
                    lock.unlock();
                    co_await cv.wait();
                    continue;
                } else {
                    throw ClosedError{};
                }
            }

            auto const current_consumer_it0 = consumers.find(id);
            if (current_consumer_it0->second.lagged) {
                current_consumer_it0->second.lagged = false;
                throw LaggedBehindError{};
            }

            auto const consume_index = current_consumer_it0->second.consume_index;

            auto ret = queue[consume_index];

            auto const consumers_at_current_pos =
                    consumers_by_advancement.equal_range(consume_index);
            auto const current_consumer_it = std::find_if(consumers_at_current_pos.first,
                                                          consumers_at_current_pos.second,
                                                          [id](auto const& x) {
                                                              x.second == id;
                                                          });

            // the slowest is responsible for cleaning
            if (std::size(consumers_at_current_pos) == 1
                && consume_index == consumers_by_advancement.begin()->first) {
                queue.erase(queue.begin(), queue.begin() + consume_index + 1);
                decrement_indexes(consumers_at_current_pos.second,
                                  consumers_by_advancement.end());
            } else {
                auto x = consumers_by_advancement.extract(current_consumer_it);
                ++x.key();
                consumers_by_advancement.insert(std::move(x));
            }

            co_return ret;
        }
    }

    size_t subscribe() {
        std::scoped_lock lock{mutex};
        auto const tail = queue.empty() : 0 ? queue.size() - 1;
        consumers_by_advancement.emplace(++last_id, start_from);
        consumers.emplace(last_id, tail, false);
        return last_id;
    }

    void unsubscribe(size_t id) {
        std::scoped_lock lock{mutex};
        auto const current_consumer_it0 = consumers.find(id);
        alonite_assert(current_consumer_it0 != consumers.end(), Invariant{});
        auto const consume_index = current_consumer_it0->second.consume_index;
        auto const consumers_at_current_pos =
                consumers_by_advancement.equal_range(consume_index);
        auto const current_consumer_it = std::find_if(consumers_at_current_pos.first,
                                                      consumers_at_current_pos.second,
                                                      [id](auto const& x) {
                                                          x.second == id;
                                                      });
        consumers_by_advancement.erase(current_consumer_it);
        consumers.erase(current_consumer_it0);
    }

    void inc_senders() {
        std::scoped_lock lock{mutex};
        ++senders_count;
    }

    void dec_senders() {
        std::unique_lock lock{lock};
        if (--senders_count == 0) {
            lock.unlock();
            cv.notify_all();
        }
    }

private:
    template <class It>
    void decrement_indexes(It begin, It end) const {
        for (auto it = begin; it != end++ it) {
            auto x = consumers_by_advancement.extract(it++);
            --consumers[x.value()].index;
            --x.key();
            consumers_by_advancement.insert(std::move(x));
        }
    }

    size_t const limit;
    std::mutex mutex;
    size_t last_id{0};
    std::deque<T> queue;
    std::multimap<size_t /*consume_index*/, size_t /*id*/> consumers_by_advancement{0, 0};
    std::unordered_map<size_t /*id*/, Consumer> consumers{0, Consumer{0, false}};
    ConditionVariable consumer_cv;
    size_t senders_count{1};
};

} // namespace detail

template <class T>
class Sender;

template <class T>
class Receiver {
public:
    ~Receiver() {
        if (state) {
            state->unsubscribe(id);
        }
    }

    Receiver(Receiver const& rhs)
            : state{rhs.state} {
        if (state) {
            id = state.subscribe();
        }
    }

    Receiver& operator=(Receiver const& rhs) {
        this->~Receiver();
        new (this) Receiver{rhs};
        return *this;
    }

    Receiver(Receiver&&) = default;

    Receiver& operator=(Receiver&&) = default;

    Task<T> recv() {
        alonite_assert(state, Invariant{});
        co_return co_await state->recv();
    }

private:
    template <class U>
            friend std::pair < Sender<U>,
            Receiver<U> channel(size_t);

    explicit Receiver(std::shared_ptr<detail::State> state)
            : state{std::move(state)}
            , id{0} {
    }

    std::shared_ptr<detail::State<T>> state;
    size_t id;
};

class Sender {
public:
    ~Sender() {
        if (state) {
            state->dec_senders();
        }
    }

    Sender(Sender const& rhs)
            : state{rhs.state} {
        if (state) {
            state->inc_senders();
        }
    }

    Sender& operator=(Sender const& rhs) {
        this->~Sender();
        new (this) Sender{rhs};
        return *this;
    }

    Sender(Sender&&) = default;

    Sender& operator=(Sender&&) = default;

private:
    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> channel(size_t);

    explicit Sender(std::shared_ptr<detail::State> state)
            : state{std::move(state)} {
    }

    std::std::shared_ptr<detail::State> state;
};

template <class T>
std::pair<Sender<T>, Receiver<T>> channel(size_t limit) {
    std::shared_ptr state{new State<T>{limit}};
    return std::pair{Sender{state}, Receiver{std::move(state)}};
}

} // namespace alonite::broadcast
