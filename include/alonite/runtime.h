#pragma once

#include "contract.h"
#include "scope_exit.h"

#include <queue>

#include <any>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stack>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// todo: std::function with non trivial function object allocates, try to avoid this

#define alonite_print(...)                                                               \
    std::format_to(std::ostreambuf_iterator{std::cout}, __VA_ARGS__)

#define feature(x, msg) x

namespace alonite {

template <class T>
concept Awaitable = requires(T x) {
    { x.await_ready() } -> std::convertible_to<bool>;
    x.await_suspend(std::declval<std::coroutine_handle<>>());
    x.await_resume();
};

constexpr inline unsigned long long operator""_KB(unsigned long long const x) {
    return 1024L * x;
}

template <class T>
std::optional<T> take(std::optional<T>& x) {
    std::optional<T> ret;
    ret.swap(x);
    return ret;
}

class TaskStack;

class Work {
public:
    explicit Work(std::function<void()>&& work) noexcept
            : work{std::move(work)} {
    }

    explicit Work(std::function<void()>&& work,
                  std::weak_ptr<TaskStack>&& canceled) noexcept
            : work{std::move(work)}
            , canceled_{std::move(canceled)} {
    }

    bool canceled() const noexcept {
        return canceled_ ? !canceled_->lock() : false;
    }

    void operator()() && {
        std::move(work)();
    }

private:
    std::function<void()> work;
    std::optional<std::weak_ptr<TaskStack>> canceled_;
};

using DelayedWork = std::pair<Work, std::chrono::steady_clock::time_point>;

struct Executor {
    static unsigned next_id() noexcept {
        static std::atomic<unsigned> id = 0;
        return ++id;
    }

    virtual void spawn(Work&& task) = 0;
    virtual void spawn(Work&& task, std::chrono::milliseconds after) = 0;
    virtual bool remove_guard(TaskStack* x) noexcept = 0;
    virtual void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) = 0;
    virtual void add_guard_group(
            unsigned id,
            std::vector<std::shared_ptr<TaskStack>> x) noexcept(false) = 0;
    virtual bool remove_guard_group(unsigned id) noexcept = 0;
    virtual void increment_external_work() = 0;
    virtual void decrement_external_work() = 0;
    virtual ~Executor() = default;
};

class TaskStack {
public:
    template <class T>
    struct Frame {
        std::coroutine_handle<T> co;
        Executor* ex;
    };

    struct ErasedFrame {
        std::coroutine_handle<> co;
        Executor* ex;
    };

    TaskStack() = default;

    explicit TaskStack(ErasedFrame&& x) noexcept(false)
            : frames{std::vector{std::move(x)}} {
    }

    TaskStack(TaskStack const&) = delete;

    /// \invariant Inv1 - one thread at a time can operate on a task stack, meaning, that
    /// pushing shouldn't involve concurrency
    void push(ErasedFrame frame) noexcept(false) {
        frames.push(frame);
    }

    /// \invariant Inv1
    ErasedFrame pop() noexcept {
        ErasedFrame ret = frames.top();
        frames.pop();
        return ret;
    }

    template </*todo: PromiseConcept*/ class T>
    Frame<T> top() const noexcept {
        return Frame<T>{
                .co = std::coroutine_handle<T>::from_address(frames.top().co.address()),
                .ex = frames.top().ex};
    }

    ErasedFrame erased_top() const noexcept {
        return frames.top();
    }

    constexpr static size_t soo_len = 24;

    /// \invariant Inv1
    template <class T>
    T take_result() noexcept(false) {
        alonite_assert(result_index != Result::none, Invariant{});

        ALONITE_SCOPE_EXIT {
            result_index = Result::none;
        };

        switch (result_index) {
        case Result::none:
            return static_cast<T>(alonite_unreachable(Invariant{}));
        case Result::exception:
            std::rethrow_exception(result_exception);
        case Result::value:
            if constexpr (std::is_void_v<T>) {
                return;
            } else {
                static_assert(std::is_nothrow_destructible_v<T>);

                ALONITE_SCOPE_EXIT {
                    destroy_value(result_value);
                    destroy_value = nullptr;
                };

                if constexpr (sizeof(T) <= soo_len) {
                    return std::move(
                            *std::launder(reinterpret_cast<T*>(&result_value.storage)));
                } else {
                    return std::move(*static_cast<T*>(result_value.ptr));
                }
            }
        }

        return static_cast<T>(alonite_unreachable(Invariant{}));
    }

    /// \invariant Inv1
    template <class T>
    void put_result(T&& val) noexcept(noexcept(std::forward<T>(val))) {
        using U = std::decay_t<T>;
        alonite_assert(result_index == Result::none, Invariant{});

        if constexpr (sizeof(T) <= soo_len) {
            new (&result_value.storage) U(std::forward<T>(val));
        } else {
            result_value.ptr = new T{std::forward<T>(val)};
        }

        destroy_value = +[](ResultType& result_value) {
            if constexpr (sizeof(T) <= soo_len) {
                std::launder(reinterpret_cast<U*>(&result_value.storage))->~U();
            } else {
                delete static_cast<U*>(result_value.ptr);
            }
        };

        result_index = Result::value;
    }

    /// \invariant Inv1
    void put_result() noexcept {
        alonite_assert(result_index == Result::none, Invariant{});
        result_index = Result::value;
    }

    /// \invariant Inv1
    void put_result(std::exception_ptr ptr) noexcept {
        alonite_assert(result_index == Result::none, Invariant{});
        result_exception = ptr;
        result_index = Result::exception;
    }

    size_t size() const noexcept {
        return frames.size();
    }

    bool empty() const noexcept {
        return frames.empty();
    }

    ~TaskStack() noexcept {
        while (!frames.empty()) {
            frames.top().co.destroy();
            frames.pop();
        }

        if (result_index == Result::value && destroy_value != nullptr) {
            destroy_value(result_value);
        }
    }

    unsigned /*const*/ tasks_to_complete = 0;
    std::atomic<unsigned> tasks_completed = 0;

    unsigned /*const*/ completed_task_index = 0;

    // No harm if it lives a bit longer, because the continuation will
    // take the value of the Stack, so basically the stack will be just a storage without
    // any user-interested destructor to run, and the storage will be released sooner or
    // later, when the continuation completes.
    std::shared_ptr<TaskStack> guard;

    // If a task is a spawned task, awaiting it should be guarded, because it is already
    // runnning. Also pushing and popping to/from the stack by runner should also be
    // guarded.
    bool complete = false;
    std::mutex mutex;

private:
    enum class Result {
        none,
        exception,
        value,
    } result_index = Result::none;
    std::exception_ptr result_exception;
    union ResultType {
        alignas(std::max_align_t) char storage[soo_len];
        void* ptr;
    } result_value;
    void (*destroy_value)(ResultType&) = nullptr;
    std::stack<ErasedFrame, std::vector<ErasedFrame>>
            frames; // todo: allocator optimization, intrusive list
};

inline thread_local Executor* current_executor = nullptr;

inline void schedule(std::shared_ptr<TaskStack>&& stack) {
    stack->erased_top().ex->spawn(
            Work{[feature(stack = std::move(stack),
                          "Have both abort signal and ready result set in JoinHandle "
                          "in case of a race condition")] {
                stack->erased_top().co.resume();
            }});
}

/// \post will not move if the `stack` is not scheduled
inline bool schedule_on_other_ex(std::shared_ptr<TaskStack>&& stack) {
    alonite_assert(stack, Invariant{});
    return stack->erased_top().ex != current_executor
        && (schedule(std::move(stack)), true);
}

struct FinalAwaiter {
    std::shared_ptr<TaskStack> stack;

    bool await_ready() noexcept {
        return schedule_on_other_ex(std::move(stack));
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> co) noexcept {
        ALONITE_SCOPE_EXIT {
            co.destroy();
        };
        alonite_assert(stack, Invariant{});
        return stack->erased_top().co;
    }

    void await_resume() noexcept {
    }
};

struct BasePromise {
    std::weak_ptr<TaskStack> stack; // todo: implement own weak ptr

    void unhandled_exception() {
        auto const stack = this->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->put_result(std::current_exception());
    }
};

template <class PromiseT>
struct ReturnValue {
    template <class U>
    void return_value(U&& val) noexcept(noexcept(std::decay_t<U>(std::forward<U>(val)))) {
        using T = typename PromiseT::ValueType;
        static_assert(std::is_convertible_v<U, T>,
                      "return expression should be convertible to return type");
        auto const stack = static_cast<PromiseT*>(this)->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->template put_result<T>(std::forward<U>(val));
    }
};

template <class PromiseT>
struct ReturnVoid {
    void return_void() noexcept {
        static_assert(std::is_void_v<typename PromiseT::ValueType>);
        auto const stack = static_cast<PromiseT*>(this)->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->put_result();
    }
};

template <class PromiseT, class Stack /*Coroutine Concept*/>
struct CreateStack {
    Stack get_return_object() noexcept(false) {
        auto const self = static_cast<PromiseT*>(this);
        auto const co = std::coroutine_handle<PromiseT>::from_promise(*self);
        Stack ret{std::make_shared<TaskStack>(
                          TaskStack::ErasedFrame{.co = co, .ex = current_executor}),
                  co};
        self->stack = ret.stack;
        return ret;
    }
};

template <class T>
class Task {
public:
    Task() = delete;

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type
            : BasePromise
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType = T;

        promise_type() = default;

        promise_type(promise_type const&) = delete;
        promise_type& operator=(promise_type const&) = delete;
        promise_type(promise_type&&) = delete;
        promise_type& operator=(promise_type&&) = delete;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            auto stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            return FinalAwaiter{std::move(stack)};
        }
    };

    bool await_ready() const noexcept {
        return false;
    }

    // This is the point where a new coroutine attaches to a existing stack of coroutines
    template <class U>
    std::coroutine_handle<promise_type> await_suspend(
            std::coroutine_handle<U> caller) noexcept(false) {
        this->stack = caller.promise().stack;
        auto const stack = this->stack.lock();
        alonite_assert(stack, Invariant{});
        alonite_assert(co, Invariant{});
        stack->push(TaskStack::ErasedFrame{
                .co = co,
                .ex = stack->erased_top().ex}); // Current coroutine is suspended, so
                                                // it is safe to modify the stack
        co.promise().stack = stack;
        return co;
    }

    T await_resume() noexcept(false) {
        return std::move(*this).get_result();
    }

    T get_result() && noexcept(false) {
        auto stack = this->stack.lock();
        alonite_assert(stack, Invariant{});
        return stack->template take_result<T>();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    std::coroutine_handle<promise_type> co;
    std::weak_ptr<TaskStack> stack;
};

class ConditionVariable {
public:
    ConditionVariable() = default;

    ConditionVariable(ConditionVariable const&) = delete;
    ConditionVariable& operator=(ConditionVariable const&) = delete;

    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    /// \note Can be called from synchronous code
    /// \note thread-safe
    bool notify_one() noexcept {
        if (auto c = [this] {
                std::optional<std::shared_ptr<TaskStack>> ret;
                std::lock_guard lock{mutex};
                if (!continuations.empty()) {
                    ret = continuations.front().lock();
                    continuations.pop_front();
                }
                return ret;
            }()) {
            if (!schedule_on_other_ex(std::move(*c))) {
                (*c)->erased_top().co.resume();
            }
            return true;
        } else {
            return false;
        }
    }

    /// \note Can be called from synchronous code
    /// \note thread-safe
    void notify_all() noexcept {
        for (auto const& c : [this] {
                 std::deque<std::weak_ptr<TaskStack>> ret;
                 std::lock_guard lock{mutex};
                 std::swap(ret, continuations);
                 return ret;
             }()) {
            if (auto x = c.lock(); x && !schedule_on_other_ex(std::move(x))) {
                x->erased_top().co.resume();
            }
        }
    }

    auto wait() {
        return Awaiter{this};
    }

private:
    struct Awaiter {
        Awaiter(ConditionVariable* cv) noexcept
                : cv{cv} {
        }

        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;

        Awaiter(Awaiter&& rhs) noexcept
                : cv{rhs.cv} {
            rhs.cv = nullptr;
        }

        Awaiter& operator=(Awaiter&& rhs) noexcept {
            this->~Awaiter();
            new (this) Awaiter{std::move(rhs)};
            return *this;
        }

        bool await_ready() const noexcept {
            return false;
        }

        void await_resume() const noexcept {
        }

        template <class U>
        void await_suspend(std::coroutine_handle<U> continuation) noexcept {
            current_executor->increment_external_work();
            std::lock_guard lock{cv->mutex};
            cv->continuations.push_back(continuation.promise().stack);
        }

        ~Awaiter() noexcept {
            if (cv) {
                current_executor->decrement_external_work();
            }
        }

        ConditionVariable* cv;
    };

    std::deque<std::weak_ptr<TaskStack>> continuations;
    std::mutex mutex;
};

struct Error : std::exception {};

template <class T = void>
struct Void {
    using WrapperT = T;
    static T take(TaskStack& stack) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return stack.template take_result<T>();
    }
};

template <>
struct Void<void> {
    using WrapperT = Void<void>;
    static Void take(TaskStack& stack) noexcept {
        stack.template take_result<void>();
        return Void{};
    }
    bool operator==(Void const&) const = default;
};

class Canceled : public Error {
public:
    Canceled() = default;

    template <class T>
    explicit Canceled(/*not forwarding*/ T&& t) noexcept(false)
            : result{std::move(t)} {
    }

    bool was_already_completed() const noexcept {
        return result.has_value();
    }

    template <class T>
    T get_result() const noexcept(false) {
        return std::any_cast<T>(result);
    }

    char const* what() const noexcept override {
        return "canceled";
    }

private:
    std::any result;
};

class TimedOut : public Error {
public:
    TimedOut() = default;

    char const* what() const noexcept override {
        return "timed out";
    }
};

/// Awaiting connects to the running task
template <class T>
struct JoinHandle { // todo: make class
    struct promise_type
            : BasePromise
            , CreateStack<promise_type, JoinHandle>
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType = T;

        std::optional<std::weak_ptr<TaskStack>> continuation;

        promise_type() = default;

        promise_type(promise_type const&) = delete;
        promise_type& operator=(promise_type const&) = delete;
        promise_type(promise_type&&) = delete;
        promise_type& operator=(promise_type&&) = delete;

        std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() const noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;

                bool await_ready() noexcept {
                    return !continuation.has_value()
                        || /* we need to suspend only to continue on other executor*/
                           schedule_on_other_ex(std::move(*continuation));
                }

                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<promise_type> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    alonite_assert(continuation && *continuation, Invariant{});
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto const stack = this->stack.lock();
            alonite_assert(stack, Invariant{});

            {
                std::scoped_lock lock{stack->mutex};
                stack->complete = true;
                stack->pop();
            }

            feature(current_executor->remove_guard(stack.get()),
                    "Have both abort signal and ready result set in JoinHandle in case "
                    "of a race condition");

            if (!continuation.has_value()) {
                return Awaiter{};
            } else if (auto continuation = this->continuation->lock()) {
                return Awaiter{std::move(continuation)};
            } else {
                return Awaiter{};
            }
        }
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    bool await_suspend(std::coroutine_handle<U> caller) const noexcept(false) {
        auto const stack = this->stack.lock();
        if (!stack) {
            return false;
        }

        std::scoped_lock lock{stack->mutex};

        if (stack->complete) {
            return false;
        } else {
            alonite_assert(co, Invariant{});
            co.promise().continuation = caller.promise().stack;
            return true;
        }
    }

    T await_resume() noexcept(false) {
        return std::move(*this).get_result();
    }

    T get_result() && noexcept(false) {
        auto const stack = this->stack.lock();
        if (!stack) {
            throw Canceled{};
        }

        if (!stack_guard) {
            throw Canceled{Void<T>::take(*stack)};
        }

        this->stack_guard.reset();

        return stack->template take_result<T>();
    }

    JoinHandle(std::shared_ptr<TaskStack> stack,
               std::coroutine_handle<promise_type> co) noexcept
            : stack{stack}
            , stack_guard{std::move(stack)}
            , co{co} {
        executor = current_executor;
    }

    JoinHandle(JoinHandle const&) = delete;
    JoinHandle& operator=(JoinHandle const&) = delete;

    JoinHandle(JoinHandle&&) = default;
    JoinHandle& operator=(JoinHandle&&) = default;

    ~JoinHandle() /*noexcept(false) todo: ensure noexcept*/ {
        if (stack_guard) {
            executor->add_guard(std::move(stack_guard));
        }
    }

    /// \note Can be called from synchronous code
    /// \note thread-safe
    bool abort() {
        if (!stack_guard) {
            return false;
        }

        std::scoped_lock lock{stack_guard->mutex};
        if (stack_guard->complete) {
            return false;
        }

        stack_guard.reset();
        return true;
    }

    Executor* executor;
    std::weak_ptr<TaskStack> stack;
    std::shared_ptr<TaskStack> stack_guard;
    std::coroutine_handle<promise_type> co; // todo:  get from the stack
};

struct TaskStackPtrHash {
    struct is_transparent;

    size_t operator()(std::shared_ptr<TaskStack> const& x) const noexcept {
        return std::hash<TaskStack*>{}(x.get());
    }

    size_t operator()(TaskStack* x) const noexcept {
        return std::hash<TaskStack*>{}(x);
    }
};

struct TaskStackPtrEqual {
    struct is_transparent;

    bool operator()(std::shared_ptr<TaskStack> const& lhs,
                    std::shared_ptr<TaskStack> const& rhs) const noexcept {
        return lhs.get() == rhs.get();
    }

    bool operator()(std::shared_ptr<TaskStack> const& lhs,
                    TaskStack* rhs) const noexcept {
        return lhs.get() == rhs;
    }

    bool operator()(TaskStack* lhs,
                    std::shared_ptr<TaskStack> const& rhs) const noexcept {
        return lhs == rhs.get();
    }

    bool operator()(TaskStack* lhs, TaskStack* rhs) const noexcept {
        return lhs == rhs;
    }
};

struct PqGreater {
    bool operator()(auto const& x, auto const& y) const noexcept {
        return x.second > y.second;
    }
};

class ThisThreadExecutor final : public Executor {
public:
    ThisThreadExecutor() = default;

    auto block_on(Awaitable auto task) noexcept(false) {
        using T = decltype(task.await_resume());

        current_executor = this;

        struct Stack {
            std::shared_ptr<TaskStack> stack;

            struct promise_type
                    : BasePromise
                    , std::conditional_t<std::is_void_v<T>,
                                         ReturnVoid<promise_type>,
                                         ReturnValue<promise_type>> {
                using ValueType [[maybe_unused]] = T;

                Stack get_return_object() noexcept(false) {
                    // todo: use CreateStack; it will need removing `co` from Stack
                    Stack ret{std::make_shared<TaskStack>(TaskStack::ErasedFrame{
                            .co = std::coroutine_handle<promise_type>::from_promise(
                                    *this),
                            .ex = current_executor})};
                    stack = ret.stack;
                    return ret;
                }

                std::suspend_never initial_suspend() const noexcept {
                    return {};
                }

                std::suspend_never final_suspend() const noexcept {
                    auto const stack = this->stack.lock();
                    alonite_assert(stack, Invariant{});
                    stack->pop();
                    return {};
                }
            };
        };

        auto t = [](auto task) -> Stack {
            co_return co_await task;
        }(std::move(task));

        while (true) {
            using std::chrono_literals::operator""ms;
            using std::chrono::steady_clock;

            do {
                std::vector<Work> tasks2;
                std::swap(tasks, tasks2);

                auto const now = steady_clock::now();
                while (!delayed_tasks.empty()
                       && (delayed_tasks.top().second <= now
                           || delayed_tasks.top().first.canceled())) {
                    tasks2.push_back(delayed_tasks.top().first);
                    delayed_tasks.pop();
                }

                for (auto& task : tasks2) {
                    std::move(task)();
                }
            } while (!tasks.empty());

            if (delayed_tasks.empty() && external_work == 0) {
                break;
            }

            auto slumber = steady_clock::now() + 10ms;
            if (!delayed_tasks.empty() && delayed_tasks.top().second < slumber) {
                slumber = delayed_tasks.top().second;
            }

            std::this_thread::sleep_until(slumber);
        }

        return t.stack->template take_result<T>();
    }

    void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) override {
        spawned_tasks.emplace(std::move(x));
    }

    void add_guard_group(unsigned id, std::vector<std::shared_ptr<TaskStack>> x) noexcept(
            false) override {
        spawned_task_groups.emplace(id, std::move(x));
    }

    bool remove_guard(TaskStack* x) noexcept override {
        if (auto const it = spawned_tasks.find(x); it != spawned_tasks.end()) {
            spawned_tasks.erase(it);
            return true;
        } else {
            return false;
        }
    }

    bool remove_guard_group(unsigned id) noexcept override {
        return spawned_task_groups.erase(id) > 0;
    }

private:
    void spawn(Work&& task) override {
        tasks.push_back(std::move(task));
    }

    void spawn(Work&& task, std::chrono::milliseconds after) override {
        delayed_tasks.emplace(std::move(task), std::chrono::steady_clock::now() + after);
    }

    void increment_external_work() override {
        ++external_work;
    }

    void decrement_external_work() override {
        --external_work;
    }

    std::vector<Work> tasks;
    std::priority_queue<DelayedWork, std::vector<DelayedWork>, PqGreater> delayed_tasks;
    std::unordered_set<std::shared_ptr<TaskStack>, TaskStackPtrHash, TaskStackPtrEqual>
            spawned_tasks;
    std::unordered_map<unsigned, std::vector<std::shared_ptr<TaskStack>>>
            spawned_task_groups;
    size_t external_work{0};
};

class ThreadPoolExecutor : public Executor {
public:
    /// \post reentrant
    auto block_on(Awaitable auto task) noexcept(false) {
        using T = decltype(task.await_resume());

        current_executor = this;

        struct Stack {
            std::shared_ptr<TaskStack> stack;

            struct promise_type
                    : BasePromise
                    , std::conditional_t<std::is_void_v<T>,
                                         ReturnVoid<promise_type>,
                                         ReturnValue<promise_type>> {
                using ValueType [[maybe_unused]] = T;

                Stack get_return_object() noexcept(false) {
                    // todo: use CreateStack; it will need removing `co` from Stack
                    Stack ret{std::make_shared<TaskStack>(TaskStack::ErasedFrame{
                            .co = std::coroutine_handle<promise_type>::from_promise(
                                    *this),
                            .ex = current_executor})};
                    stack = ret.stack;
                    return ret;
                }

                std::suspend_never initial_suspend() const noexcept {
                    return {};
                }

                std::suspend_never final_suspend() const noexcept {
                    auto const stack = this->stack.lock();
                    alonite_assert(stack, Invariant{});
                    stack->pop();
                    return {};
                }
            };
        };

        auto t = [](auto task) -> Stack {
            co_return co_await task;
        }(std::move(task));

        {
            active_threads.fetch_add(1, std::memory_order_relaxed);
            ALONITE_SCOPE_EXIT {
                active_threads.fetch_sub(1, std::memory_order_relaxed);
                std::atomic_notify_all(&active_threads);
            };
            while (true) {
                using std::chrono_literals::operator""ms;
                using std::chrono::steady_clock;

                std::vector<Work> tasks2;

                do {
                    {
                        std::lock_guard lock{mutex};

                        auto const n = std::min(32ul, tasks.size());
                        tasks2.assign(std::make_move_iterator(begin(tasks)),
                                      std::make_move_iterator(begin(tasks) + n));
                        tasks.erase(begin(tasks), begin(tasks) + n);

                        auto const now = steady_clock::now();
                        while (!delayed_tasks.empty()
                               && (delayed_tasks.top().second <= now
                                   || delayed_tasks.top().first.canceled())) {
                            tasks2.push_back(delayed_tasks.top().first);
                            delayed_tasks.pop();
                        }

                        // need help
                        if (!tasks.empty()) {
                            cv.notify_one();
                        }
                    }

                    for (auto& task : tasks2) {
                        std::move(task)();
                    }
                } while (!tasks2.empty());

                std::unique_lock lock{mutex};
                if (external_work.load(std::memory_order_relaxed) == 0 && tasks.empty()
                    && delayed_tasks.empty()) {
                    break;
                } else if (!delayed_tasks.empty()) {
                    auto const tmp = delayed_tasks.top().second;
                    cv.wait_until(lock, tmp);
                } else {
                    cv.wait(lock);
                }
            }
        }

        while (auto const x = active_threads.load(std::memory_order_relaxed)) {
            cv.notify_all();
            std::atomic_wait(&active_threads, x);
        }

        return t.stack->template take_result<T>();
    }

    void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) override {
        std::lock_guard lock{mutex};
        spawned_tasks.emplace(std::move(x));
    }

    void add_guard_group(unsigned id, std::vector<std::shared_ptr<TaskStack>> x) noexcept(
            false) override {
        std::lock_guard lock{mutex};
        spawned_task_groups.emplace(id, std::move(x));
    }

    bool remove_guard(TaskStack* x) noexcept override {
        std::lock_guard lock{mutex};
        if (auto const it = spawned_tasks.find(x); it != spawned_tasks.end()) {
            spawned_tasks.erase(it);
            return true;
        } else {
            return false;
        }
    }

    bool remove_guard_group(unsigned id) noexcept override {
        std::lock_guard lock{mutex};
        return spawned_task_groups.erase(id) > 0;
    }

    void spawn(Work&& task) override {
        std::lock_guard lock{mutex};
        tasks.push_back(std::move(task));
        cv.notify_one();
    }

    void spawn(Work&& task, std::chrono::milliseconds after) override {
        std::lock_guard lock{mutex};
        delayed_tasks.emplace(std::move(task), std::chrono::steady_clock::now() + after);
        cv.notify_one();
    }

    void increment_external_work() override {
        external_work.fetch_add(1, std::memory_order_relaxed);
    }

    void decrement_external_work() override {
        external_work.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Work> tasks;
    std::priority_queue<DelayedWork, std::deque<DelayedWork>, PqGreater> delayed_tasks;
    std::unordered_set<std::shared_ptr<TaskStack>, TaskStackPtrHash, TaskStackPtrEqual>
            spawned_tasks;
    std::unordered_map<unsigned, std::vector<std::shared_ptr<TaskStack>>>
            spawned_task_groups;
    std::atomic<size_t> external_work{0};
    std::atomic<unsigned> active_threads{0};
};

class Sleep {
public:
    explicit Sleep(std::chrono::milliseconds dur) noexcept
            : dur{dur} {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template <class T>
    void await_suspend(std::coroutine_handle<T> caller) {
        auto caller_stack = caller.promise().stack.lock();
        alonite_assert(caller_stack, Invariant{});
        caller_stack->erased_top().ex->spawn(
                Work{[caller_stack = std::weak_ptr{caller_stack}]() mutable {
                         if (auto const r = caller_stack.lock()) {
                             r->erased_top().co.resume();
                         }
                     },
                     std::weak_ptr{caller_stack}},
                dur);
    }

    void await_resume() noexcept {
    }

private:
    std::chrono::milliseconds dur;
};

template <class TaskT /*Coroutine Concept*/>
class Timeout {
public:
    explicit Timeout(std::chrono::milliseconds duration, TaskT&& task) noexcept
            : dur{duration}
            , task{std::move(task)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        using T = typename TaskT::promise_type::ValueType;

        struct Stack {
            struct promise_type
                    : BasePromise
                    , CreateStack<promise_type, Stack>
                    , std::conditional_t<std::is_void_v<T>,
                                         ReturnVoid<promise_type>,
                                         ReturnValue<promise_type>> {
                using ValueType [[maybe_unused]] = T;

                std::weak_ptr<TaskStack> continuation;

                std::suspend_always initial_suspend() const noexcept {
                    return {};
                }

                auto final_suspend() const noexcept {
                    struct Awaiter {
                        std::optional<std::shared_ptr<TaskStack>> continuation;

                        bool await_ready() noexcept {
                            return /* we need to suspend only to continue on other
                                      executor*/
                                    !continuation.has_value()
                                    || schedule_on_other_ex(std::move(*continuation));
                        }

                        std::coroutine_handle<> await_suspend(
                                std::coroutine_handle<promise_type> co) noexcept {
                            ALONITE_SCOPE_EXIT {
                                co.destroy();
                            };
                            return (*continuation)->erased_top().co;
                        }

                        void await_resume() noexcept {
                        }
                    };

                    auto stack = this->stack.lock();
                    alonite_assert(stack, Invariant{});
                    stack->pop();
                    alonite_assert(stack->size() == 0, Invariant{});

                    auto continuation = this->continuation.lock();
                    return (continuation && current_executor->remove_guard(stack.get()))
                                 ? Awaiter{std::move(
                                           (continuation->guard = std::move(stack),
                                            continuation))}
                                 : Awaiter{};
                }
            };

            std::shared_ptr<TaskStack> stack;
            std::coroutine_handle<promise_type>
                    co; // todo:? is available as first frame in the stack
        };

        auto task_wrapper = [](auto task) -> Stack {
            co_return co_await task;
        }(std::move(task));
        this->stack = task_wrapper.stack;
        task_wrapper.co.promise().continuation = caller.promise().stack;
        current_executor->add_guard(std::shared_ptr{task_wrapper.stack});
        current_executor->spawn(Work{[stack = std::weak_ptr{task_wrapper.stack},
                                      continuation = caller.promise().stack]() mutable {
                                         if (auto const x = stack.lock();
                                             x
                                             && current_executor->remove_guard(x.get())) {
                                             cancel_and_continue(std::move(stack),
                                                                 std::move(continuation));
                                         }
                                     },
                                     std::weak_ptr{task_wrapper.stack}},
                                this->dur);

        return task_wrapper.co;
    }

    typename TaskT::promise_type::ValueType await_resume() noexcept(false) {
        auto const stack = this->stack.lock();
        if (!stack) {
            throw TimedOut{};
        }
        return stack->template take_result<typename TaskT::promise_type::ValueType>();
    }

private:
    static void cancel_and_continue(std::weak_ptr<TaskStack>&& stack,
                                    std::weak_ptr<TaskStack>&& continuation) {
        if (stack.lock()) {
            current_executor->spawn(Work{
                    [stack, continuation = std::move(continuation)]() mutable {
                        cancel_and_continue(std::move(stack), std::move(continuation));
                    },
                    std::move(stack)});
        } else if (auto x = continuation.lock();
                   x
                   && !schedule_on_other_ex(
                           /*not moved if not scheduled*/ std::move(x))) {
            x->erased_top().co.resume();
        }
    }

private:
    std::chrono::milliseconds dur;
    TaskT task;
    std::weak_ptr<TaskStack> stack;
};

template <class T>
struct WhenAllStack {
    struct promise_type
            : BasePromise
            , CreateStack<promise_type, WhenAllStack>
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType [[maybe_unused]] = T;

        std::weak_ptr<TaskStack> continuation;

        std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;

                bool await_ready() noexcept {
                    if (!continuation.has_value()
                        || ++(*continuation)->tasks_completed
                                   < (*continuation)->tasks_to_complete) {
                        return true;
                    }
                    return schedule_on_other_ex(std::move(*continuation));
                }

                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto const stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            alonite_assert(stack->size() == 0, Invariant{});

            auto continuation = this->continuation.lock();
            return continuation ? Awaiter{std::move(continuation)} : Awaiter{};
        }
    };

    std::shared_ptr<TaskStack> stack;
    std::coroutine_handle<promise_type>
            co; // todo:? is available as first frame in the stack
};

template <class... TaskT /*Coroutine Concept*/>
class WhenAll {
public:
    static_assert(sizeof...(TaskT) > 0);

    explicit WhenAll(TaskT&&... tasks) noexcept
            : tasks{std::move(tasks)...} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        auto const continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = sizeof...(TaskT);
            x->tasks_completed = 0;
        }

        // create stacks
        auto task_wrappers = std::apply(
                []<typename... X>(X&&... task) {
                    return std::tuple{
                            [](auto task)
                                    -> WhenAllStack<typename X::promise_type::ValueType> {
                                co_return co_await task;
                            }(std::move(task))...};
                },
                std::move(tasks));

        // set continuation
        this->stacks = std::apply(
                [continuation](auto&... stack_wrapper) {
                    return std::array{
                            (stack_wrapper.co.promise().continuation = continuation,
                             stack_wrapper.stack)...};
                },
                task_wrappers);

        return std::apply(
                [](auto const& first, auto&&... other) {
                    // schedule
                    [](...) {
                    }((schedule(std::move(other.stack)), 0)...);

                    // resume immediately
                    return first.co;
                },
                std::move(task_wrappers));
    }

    std::tuple<typename Void<typename TaskT::promise_type::ValueType>::WrapperT...>
    await_resume() noexcept(false) {
        return [this]<size_t... Is>(std::index_sequence<Is...>) {
            return std::tuple{
                    Void<typename std::tuple_element_t<Is, std::tuple<TaskT...>>::
                                 promise_type::ValueType>::take(*stacks[Is])...};
        }(std::make_index_sequence<sizeof...(TaskT)>());
    }

private:
    std::tuple<TaskT...> tasks;
    std::array<std::shared_ptr<TaskStack>, sizeof...(TaskT)> stacks;
};

template <class TaskT /*Coroutine Concept*/>
class WhenAllDyn {
public:
    explicit WhenAllDyn(std::vector<TaskT>&& tasks) noexcept
            : tasks{std::move(tasks)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) noexcept(
            false) {
        auto const continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = this->tasks.size();
            x->tasks_completed = 0;
        }

        stacks.resize(tasks.size());
        std::transform(
                std::make_move_iterator(begin(tasks)),
                std::make_move_iterator(end(tasks)),
                begin(stacks),
                [continuation](TaskT&& task) {
                    auto ret = [](auto task)
                            -> WhenAllStack<typename TaskT::promise_type::ValueType> {
                        co_return co_await task;
                    }(std::move(task));
                    ret.co.promise().continuation = continuation;
                    return std::move(ret.stack);
                });

        for (unsigned i = 1; i < stacks.size(); ++i) {
            schedule(std::shared_ptr(stacks[i]));
        }

        return stacks[0]->erased_top().co;
    }

    std::vector<typename Void<typename TaskT::promise_type::ValueType>::WrapperT>
    await_resume() noexcept(false) {
        std::vector<typename Void<typename TaskT::promise_type::ValueType>::WrapperT> ret(
                stacks.size());
        std::transform(begin(stacks), end(stacks), begin(ret), [](auto const& stack) {
            return Void<typename TaskT::promise_type::ValueType>::take(*stack);
        });
        return ret;
    }

private:
    std::vector<TaskT> tasks;
    std::vector<std::shared_ptr<TaskStack>> stacks;
};

template <class T>
struct WhenAnyStack {
    struct promise_type
            : BasePromise
            , CreateStack<promise_type, WhenAnyStack>
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType [[maybe_unused]] = T;

        std::weak_ptr<TaskStack> continuation;
        unsigned index;
        unsigned id;

        std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;
                unsigned index{};

                bool await_ready() noexcept {
                    if (continuation.has_value()) {
                        (*continuation)->completed_task_index = index;
                        return schedule_on_other_ex(std::move(*continuation));
                    }
                    return true;
                }

                // todo: reuse
                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    // todo:? maybe release
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            alonite_assert(stack->size() == 0, Invariant{});

            auto continuation = this->continuation.lock();

            return (continuation && current_executor->remove_guard_group(id))
                         ? Awaiter{std::move((continuation->guard = std::move(stack),
                                              continuation)),
                                   index}
                         : Awaiter{};
        }
    };

    std::shared_ptr<TaskStack> stack;
    std::coroutine_handle<promise_type>
            co; // todo:? is available as first frame in the stack
};

template <class... TaskT /*Coroutine Concept*/>
class WhenAny {
public:
    static_assert(sizeof...(TaskT) > 0);

    explicit WhenAny(TaskT&&... tasks) noexcept
            : tasks{std::move(tasks)...} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) noexcept(
            false) {
        continuation = caller.promise().stack;

        {
            auto const x = continuation.lock();
            alonite_assert(x, Invariant{});
            x->tasks_to_complete = x->completed_task_index = sizeof...(TaskT);
        }

        // create stacks
        auto task_wrappers = std::apply(
                []<typename... X>(X&&... task) {
                    return std::tuple{
                            [](auto task)
                                    -> WhenAnyStack<typename X::promise_type::ValueType> {
                                co_return co_await task;
                            }(std::move(task))...};
                },
                std::move(tasks));

        auto const id = current_executor->next_id();

        // Populate index, group id and continuation
        // Array for us - because its static. Vec for guarding, because it is dynamic. The
        // contain copies of the same thing.
        auto arr_vec = std::apply(
                [this, id](auto&... stack_wrapper) {
                    unsigned i = 0;
                    return std::pair{std::array{(stack_wrapper.co.promise().continuation =
                                                         continuation,
                                                 stack_wrapper.co.promise().index = i++,
                                                 stack_wrapper.co.promise().id = id,
                                                 std::weak_ptr{stack_wrapper.stack})...},
                                     std::vector{stack_wrapper.stack...}};
                },
                task_wrappers);

        // save
        this->stacks = std::move(arr_vec).first;
        current_executor->add_guard_group(id, std::move(arr_vec).second);

        // start
        return std::apply(
                [](auto const& first, auto&&... other) {
                    [](...) {
                    }((schedule(std::move(other.stack)), 0)...);
                    return first.co;
                },
                std::move(task_wrappers));
    }

    using ReturnType = std::variant<
            typename Void<typename TaskT::promise_type::ValueType>::WrapperT...>;

    ReturnType await_resume() noexcept(false) {
        auto const continuation = this->continuation.lock();
        alonite_assert(continuation, Invariant{});

        std::optional<ReturnType> ret;
        [&]<auto... Is>(std::index_sequence<Is...>) {
            [](...) {
            }(continuation->completed_task_index == Is
              && (alonite_assert(stacks[Is].lock(), Invariant{}),
                  ret = ReturnType{std::in_place_index<Is>,
                                   Void<typename std::tuple_element_t<
                                           Is,
                                           std::tuple<TaskT...>>::promise_type::
                                                ValueType>::take(*stacks[Is].lock())},
                  true)...);
        }(std::make_index_sequence<sizeof...(TaskT)>());

        return std::move(*ret);
    }

private:
    std::tuple<TaskT...> tasks;
    std::weak_ptr<TaskStack> continuation;
    std::array<std::weak_ptr<TaskStack>, sizeof...(TaskT)> stacks;
};

template <class TaskT /*Coroutine Concept*/>
class WhenAnyDyn {
public:
    explicit WhenAnyDyn(std::vector<TaskT>&& tasks) noexcept
            : tasks{std::move(tasks)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) noexcept(
            false) {
        continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = x->completed_task_index = this->tasks.size();
        }

        auto const id = current_executor->next_id();
        std::vector<std::shared_ptr<TaskStack>> stacks(tasks.size());
        std::transform(
                std::make_move_iterator(begin(tasks)),
                std::make_move_iterator(end(tasks)),
                begin(stacks),
                [this, i = size_t(0), id](TaskT&& task) mutable {
                    auto ret = [](auto task)
                            -> WhenAnyStack<typename TaskT::promise_type::ValueType> {
                        co_return co_await task;
                    }(std::move(task));
                    ret.co.promise().continuation = continuation;
                    ret.co.promise().index = i++;
                    ret.co.promise().id = id;
                    return std::move(ret.stack);
                });

        this->stacks.resize(stacks.size());
        std::transform(
                begin(stacks), end(stacks), begin(this->stacks), [](auto const& x) {
                    return std::weak_ptr{x};
                });

        current_executor->add_guard_group(id, stacks);

        for (unsigned i = 1; i < stacks.size(); ++i) {
            schedule(std::shared_ptr(stacks[i]));
        }

        return stacks[0]->erased_top().co;
    }

    using ReturnType = typename Void<typename TaskT::promise_type::ValueType>::WrapperT;

    ReturnType await_resume() noexcept(false) {
        auto const continuation = this->continuation.lock();
        alonite_assert(continuation, Invariant{});
        return Void<typename TaskT::promise_type::ValueType>::take(
                *stacks[continuation->completed_task_index].lock());
    }

private:
    std::vector<TaskT> tasks;
    std::weak_ptr<TaskStack> continuation;
    std::vector<std::weak_ptr<TaskStack>> stacks;
};

inline auto spawn(Awaitable auto&& task) noexcept(false)
        -> JoinHandle<decltype(task.await_resume())> {
    using T = decltype(task.await_resume());
    auto t = [](auto task) -> JoinHandle<T> {
        co_return co_await task;
    }(std::move(task));
    return t;
}

class Yield {
public:
    bool await_ready() const noexcept {
        return false;
    }

    template <class T>
    void await_suspend(std::coroutine_handle<T> caller) noexcept(false) {
        current_executor->spawn(Work{[continuation = caller.promise().stack] {
                                         if (auto const x = continuation.lock()) {
                                             x->erased_top().co.resume();
                                         }
                                     },
                                     std::weak_ptr{caller.promise().stack}});
    }

    void await_resume() const noexcept {
    }
};

template <Awaitable A>
class WithExecutor {
    using R = decltype(std::declval<A>().await_resume());

    struct Stack {
        struct promise_type
                : BasePromise
                , std::conditional_t<std::is_void_v<R>,
                                     ReturnVoid<promise_type>,
                                     ReturnValue<promise_type>> {
            using ValueType = R;

#if defined(__clang__)
            promise_type(A const&, std::weak_ptr<TaskStack> x, Executor* ex)
                    : BasePromise{std::move(x)}
                    , ex{ex} {
            }
#else
            template <class Z>
            promise_type(Z const&, A const&, std::weak_ptr<TaskStack> x, Executor* ex)
                    : BasePromise{std::move(x)}
                    , ex{ex} {
            }
#endif

            promise_type(promise_type const&) = delete;
            promise_type& operator=(promise_type const&) = delete;
            promise_type(promise_type&&) = delete;
            promise_type& operator=(promise_type&&) = delete;

            Stack get_return_object() noexcept {
                auto const stack = this->stack.lock();
                auto const co = std::coroutine_handle<promise_type>::from_promise(*this);
                alonite_assert(stack, Invariant{}, "`caller` can't be dead and await us");
                stack->push(TaskStack::ErasedFrame{.co = co, .ex = ex});
                return Stack{};
            }

            std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            auto final_suspend() const noexcept {
                auto stack = this->stack.lock();
                alonite_assert(stack, Invariant{});
                stack->pop();
                return FinalAwaiter{std::move(stack)};
            }

            Executor* ex;
        };
    };

public:
    explicit WithExecutor(Executor* ex, A&& aw) noexcept
            : ex{ex}
            , aw{std::move(aw)} {
    }

    ~WithExecutor() {
        take(prev_ex).value()->decrement_external_work();
    }

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    void await_suspend(std::coroutine_handle<U> caller) noexcept(false) {
        stack = caller.promise().stack;
        auto stack = this->stack->lock();
        alonite_assert(stack, Invariant{}, "`caller` can't be dead and await us");

        prev_ex = stack->erased_top().ex;
        (*prev_ex)->increment_external_work();

        [](auto aw, auto const&, auto const&) -> Stack {
            co_return co_await aw;
        }(std::move(aw).value(), *this->stack, ex);

        schedule(std::move(stack));
    }

    R await_resume() noexcept {
        auto stack = take(this->stack).value().lock();
        alonite_assert(stack, Invariant{});
        return stack->template take_result<R>();
    }

private:
    Executor* ex;
    std::optional<A> aw;
    std::optional<std::weak_ptr<TaskStack>> stack;
    std::optional<Executor*> prev_ex;
};

} // namespace alonite
