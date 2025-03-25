#pragma once

#include "contract.h"
#include "runtime.h"

#include <QObject>
#include <QTimer>

namespace alonite {

class QExecutor
        : public virtual Executor
        , public virtual UnsyncStackOwnerAndExternalWorkImpl {
public:
    QExecutor() {
        current_executor = this;
    }

    void spawn(Work&& w) override {
        QTimer::singleShot(0, std::move(w));
    }

    void spawn(Work&& w, std::chrono::milliseconds after) override {
        QTimer::singleShot(after, std::move(w));
    }
};

struct Disconnected : Error {
    char const* what() const noexcept override {
        return "Disconnected";
    }
};

/// Signal awaiter.
/// It is a lightweight copiable object, that connects the signal on constructor
/// and disconnects when the last copy of it is destructed. When the sender object is
/// destructed or disconnected while something is awaiting this, the upstream will be continued with
/// the exception `Disconnected`.
template <class T, class... Args>
class [[nodiscard]] Signal {
public:
    Signal(T& obj, void (T::*sig)(Args...)) {
        std::shared_ptr<Receiver> state{new Receiver{}};
        state->set_connection(QObject::connect(&obj, sig, [state](Args const&... args) {
            (*state)(args...);
        }));
        this->state = state;
    }

    Signal(Signal const&) = default;
    Signal& operator=(Signal const&) = default;

    Signal(Signal&&) = default;
    Signal& operator=(Signal&&) = default;

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    void await_suspend(std::coroutine_handle<U> caller) {
        if (auto const state = this->state.lock()) {
            auto stack = caller.promise().stack.lock();
            alonite_assert(stack, Invariant{});
            stack->erased_top().ex->increment_external_work();
            this->caller = caller.promise().stack;
            state->add_receiver(std::weak_ptr{caller.promise().stack});
        } else {
            throw Disconnected{};
        }
    }

    std::tuple<Args...> await_resume() {
        auto const stack = caller.value().lock();
        alonite_assert(stack, Invariant{});
        return stack->take_result<std::tuple<Args...>>();
    }

private:
    class Receiver {
    public:
        ~Receiver() {
            if (auto connection = take(this->connection)) {
                QObject::disconnect(*connection);
            }

            std::vector<std::weak_ptr<TaskStack>> coros;

            {
                std::scoped_lock lock{mutex};
                std::swap(coros, this->coros);
            }

            for (std::weak_ptr<TaskStack> const& coro : coros) {
                if (auto const x = coro.lock()) {
                    x->put_result(std::make_exception_ptr(Disconnected{}));
                    auto const ex = x->erased_top().ex;
                    schedule(std::shared_ptr{x});
                    ex->decrement_external_work();
                }
            }
        }

        void operator()(Args const&... args) {
            std::vector<std::weak_ptr<TaskStack>> coros;

            {
                std::scoped_lock lock{mutex};
                std::swap(coros, this->coros);
            }

            for (std::weak_ptr<TaskStack> const& coro : coros) {
                if (auto x = coro.lock()) {
                    x->put_result(std::tuple(args...));
                    auto const ex = x->erased_top().ex;
                    schedule(std::shared_ptr{x});
                    ex->decrement_external_work();
                }
            }
        }

        void set_connection(QMetaObject::Connection&& connection) {
            this->connection = std::move(connection);
        }

        void add_receiver(std::weak_ptr<TaskStack>&& receiver) {
            std::scoped_lock lock{mutex};
            coros.push_back(std::move(receiver));
        }

        std::optional<QMetaObject::Connection> connection;
        std::vector<std::weak_ptr<TaskStack>> coros;
        std::mutex mutex;
    };

    std::optional<std::weak_ptr<TaskStack>> caller;
    std::weak_ptr<Receiver> state;
};

} // namespace alonite
