#include <alonite/runtime.h>

#include <catch2/catch_all.hpp>

#include <string_view>

using namespace alonite;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace std::chrono;

TEST_CASE("value result", "[ThisThreadExecutor::block_on]") {
    ThisThreadExecutor executor;
    auto const x = executor.block_on([&]() -> Task<int> {
        co_return 1 + 1;
    }());
    REQUIRE(x == 2);
}

TEST_CASE("exception result", "[ThisThreadExecutor::block_on]") {
    ThisThreadExecutor executor;
    auto t = true;
    REQUIRE_THROWS_AS((executor.block_on([&]() -> Task<int> {
                          if (t) {
                              throw 1;
                          }
                          co_return 1 + 1;
                      }())),
                      int);
}

TEST_CASE("void result", "[ThisThreadExecutor::block_on]") {
    ThisThreadExecutor executor;
    bool executed = false;
    executor.block_on([&]() -> Task<void> {
        ALONITE_SCOPE_EXIT {
            executed = true;
        };
        co_return;
    }());
    REQUIRE(executed);
}

TEST_CASE("destruction order should be natural", "[ThisThreadExecutor::block_on]") {
    ThisThreadExecutor executor;

    std::vector<std::string_view> events;

    struct Add {
        [[maybe_unused]] ~Add() {
            events.push_back(x);
        }

        std::vector<std::string_view>& events;
        std::string_view x;
    };

    executor.block_on([&](std::shared_ptr<Add>) -> Task<void> {
        co_await [&](std::shared_ptr<Add>) -> Task<void> {
            co_return;
        }(std::shared_ptr<Add>(new Add{events, "x"}));
    }(std::shared_ptr<Add>(new Add{events, "y"})));

    REQUIRE(events == (std::vector{"x"sv, "y"sv}));
}

TEST_CASE("await for result", "[spawn]") {
    ThisThreadExecutor executor;
    executor.block_on([&]() -> Task<void> {
        auto x = co_await spawn([]() -> Task<int> {
            co_return 1 + 1;
        }());
        REQUIRE(x == 2);
        co_return;
    }());
}

TEST_CASE("ignore result", "[Sleep]") {
    ThisThreadExecutor executor;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await Sleep{5ms};
        co_return;
    }());
    auto const end = steady_clock::now();
    REQUIRE(5ms <= end - start);
    REQUIRE(end - start < 10ms);
}

TEST_CASE("should set result", "[Sleep]") {
    ThisThreadExecutor executor;
    auto const start = steady_clock::now();
    executor.block_on(Sleep{5ms});
    auto const end = steady_clock::now();
    REQUIRE(5ms <= end - start);
    REQUIRE(end - start < 10ms);
}

TEST_CASE("ignore result", "[spawn]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        spawn([&]() -> Task<int> {
            run = true;
            co_return 1 + 1;
        }());
        co_return;
    }());
    REQUIRE(run);
}

TEST_CASE("sleep in parallel", "[spawn]") {
    ThisThreadExecutor executor;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        spawn([&]() -> Task<void> {
            co_await Sleep{10ms};
        }());
        spawn([&]() -> Task<void> {
            co_await Sleep{10ms};
        }());
        co_return;
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(10ms <= elapsed);
    REQUIRE(elapsed <= 12ms);
}

TEST_CASE("use some sleep to actually enter the block_on loop", "[spawn]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        spawn([](bool& run) -> Task<int> {
            co_await Sleep{5ms};
            run = true;
            co_return 1 + 1;
        }(run));
        co_return;
    }());
    REQUIRE(run);
}

TEST_CASE("abort", "[spawn]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        auto handle = spawn([](bool& run) -> Task<void> {
            co_await Sleep{5ms};
            run = true;
            co_return;
        }(run));

        co_await Sleep{1ms};
        REQUIRE(handle.abort());

        try {
            co_await handle;
        } catch (Canceled const&) {
            exception = true;
        }

        co_return;
    }());
    REQUIRE(!run);
    REQUIRE(exception);
}

TEST_CASE("abort already ready task does nothing", "[spawn]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        auto handle = spawn([](bool& run) -> Task<void> {
            run = true;
            co_return;
        }(run));

        co_await Sleep{1ms};
        REQUIRE(!handle.abort());

        try {
            co_await handle;
        } catch (Canceled const&) {
            exception = true;
        }

        co_return;
    }());
    REQUIRE(run);
    REQUIRE(!exception);
}

TEST_CASE("the task was already completed by the abort time", "[spawn]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        JoinHandle<void>* hack = nullptr;
        JoinHandle<void> handle = spawn([](bool& run, auto const& hack) -> Task<void> {
            run = true;

            co_await Sleep{2ms};
            alonite_assert(hack, Invariant{});

            // at this point, we are practically complete
            REQUIRE(hack->abort());

            co_return;
        }(run, hack));
        hack = &handle;

        co_await Sleep{1ms};

        try {
            co_await handle;
        } catch (Canceled const& x) {
            REQUIRE(x.was_already_completed());
            exception = true;
        }

        co_return;
    }());
    REQUIRE(run);
    REQUIRE(exception);
}

TEST_CASE(
        "spawn a task and wait on cv in it, then after 5ms notify it from the outer task",
        "[ConditionVariable]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        ConditionVariable cv;

        spawn([](int* counter, ConditionVariable* cv) -> Task<void> {
            auto const start = steady_clock::now();

            co_await cv->wait();

            auto const elapsed = steady_clock::now() - start;

            *counter += 2;

            REQUIRE(5ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 6ms);
#endif

            co_return;
        }(&counter, &cv));

        co_await Sleep{5ms};
        cv.notify_one();
    }());
    REQUIRE(counter == 2);
}

TEST_CASE(
        "spawn two tasks and wait on cv in it, then after 5ms notify one of them, then "
        "after 5ms the other",
        "[ConditionVariable]") {
    ThisThreadExecutor executor;
    std::vector<std::string_view> events;
    executor.block_on([&]() -> Task<void> {
        ConditionVariable cv;

        spawn([](auto& events, ConditionVariable* cv) -> Task<void> {
            auto const start = steady_clock::now();

            co_await cv->wait();

            auto const elapsed = steady_clock::now() - start;

            events.push_back("task 1 awakened");

            REQUIRE(5ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 6ms);
#endif

            co_return;
        }(events, &cv));

        spawn([](auto& events, ConditionVariable* cv) -> Task<void> {
            auto const start = steady_clock::now();

            co_await cv->wait();

            auto const elapsed = steady_clock::now() - start;

            events.push_back("task 2 awakened");

            REQUIRE(10ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 11ms);
#endif

            co_return;
        }(events, &cv));

        co_await Sleep{5ms};
        cv.notify_one();

        co_await Sleep{5ms};
        cv.notify_one();
    }());
    REQUIRE(events == (std::vector{"task 1 awakened"sv, "task 2 awakened"sv}));
}

TEST_CASE("spawn two tasks and wait on cv in it, then after 5ms notify both of them",
          "[ConditionVariable]") {
    ThisThreadExecutor executor;
    std::vector<std::string_view> events;
    executor.block_on([&]() -> Task<void> {
        ConditionVariable cv;

        spawn([](auto& events, ConditionVariable* cv) -> Task<void> {
            auto const start = steady_clock::now();

            co_await cv->wait();

            auto const elapsed = steady_clock::now() - start;

            events.push_back("task 1 awakened");

            REQUIRE(5ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 6ms);
#endif

            co_return;
        }(events, &cv));

        spawn([](auto& events, ConditionVariable* cv) -> Task<void> {
            auto const start = steady_clock::now();

            co_await cv->wait();

            auto const elapsed = steady_clock::now() - start;

            events.push_back("task 2 awakened");

            REQUIRE(5ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 6ms);
#endif

            co_return;
        }(events, &cv));

        co_await Sleep{5ms};
        cv.notify_all();
    }());
    REQUIRE(events == (std::vector{"task 1 awakened"sv, "task 2 awakened"sv}));
}

TEST_CASE(
        "a timeout on a different executor on upstream cancels the coroutine and "
        "decrements external work on the correct executor",
        "[ConditionVariable]") {
    ThisThreadExecutor exec;

    exec.block_on([&exec]() -> Task<void> {
        ThreadPoolExecutor pool_exec;
        ConditionVariable cv;

        std::thread t1{[&] {
            pool_exec.block_on([](auto& cv) -> Task<void> {
                cv.notify_one();
                co_await cv.wait();
            }(cv));
        }};

        co_await cv.wait();

        std::thread t2{[&] {
            pool_exec.block_on(Yield{});
        }};

        ALONITE_SCOPE_EXIT {
            t1.join();
            t2.join();
        };

        try {
            co_await Timeout{10ms, WithExecutor{&pool_exec, cv.wait()}};
        } catch (TimedOut const&) {
        }

        cv.notify_all();
    }());
}

TEST_CASE("the coroutine is on time", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const x = co_await Timeout{2ms, []() -> Task<int> {
                                            co_return 1;
                                        }()};
        ++counter;
        REQUIRE(x == 1);
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("the coroutine is on time with some sleep", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const x = co_await Timeout{2ms, []() -> Task<int> {
                                            co_await Sleep{1ms};
                                            co_return 1;
                                        }()};
        ++counter;
        REQUIRE(x == 1);
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("the coroutine is late", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        try {
            co_await Timeout{1ms, []() -> Task<int> {
                                 co_await Sleep{2ms};
                                 co_return 1;
                             }()};
            counter = 1;
        } catch (TimedOut const&) {
            counter = 2;
        }
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("void task, the coroutine is on time", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        co_await Timeout{2ms, []() -> Task<void> {
                             co_await Sleep{1ms};
                             co_return;
                         }()};
        ++counter;
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("void task, the coroutine is late", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        try {
            co_await Timeout{1ms, []() -> Task<void> {
                                 co_await Sleep{2ms};
                                 co_return;
                             }()};
            counter = 1;
        } catch (TimedOut const&) {
            counter = 2;
        }
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one void task", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<void> {
            x += 1;
            co_return;
        }(counter)};
        counter += 2;
        REQUIRE(tmp == std::tuple{Void<>{}});
    }());
    REQUIRE(counter == 3);
}

TEST_CASE("one int task", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
            x += 1;
            co_return 2;
        }(counter)};
        counter += 2;
        REQUIRE(tmp == std::tuple{2});
    }());
    REQUIRE(counter == 3);
}

TEST_CASE("one int task and one void", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
                                              x += 1;
                                              co_return 2;
                                          }(counter1),
                                          [](auto& x) -> Task<void> {
                                              x += 1;
                                              co_return;
                                          }(counter2)};
        counter1 += 2;
        REQUIRE(tmp == std::tuple{2, Void<>{}});
    }());
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
}

TEST_CASE("one int task and one void with sleeps", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
                                              co_await Sleep{1ms};
                                              x += 1;
                                              co_return 2;
                                          }(counter1),
                                          [](auto& x) -> Task<void> {
                                              co_await Sleep{2ms};
                                              x += 1;
                                              co_return;
                                          }(counter2)};
        counter1 += 2;
        REQUIRE(tmp == std::tuple{2, Void<>{}});
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
    REQUIRE(2ms < elapsed);
    REQUIRE(elapsed < 3ms);
}

TEST_CASE("check tasks execute simultaneously", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 2;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await WhenAll{[](auto& x) -> Task<void> {
                             co_await Sleep{2ms};
                             x *= 2;
                             co_await Sleep{2ms};
                             x *= 4;
                         }(counter),
                         [](auto& x) -> Task<void> {
                             co_await Sleep{2ms};
                             x *= 3;
                             co_await Sleep{2ms};
                             x *= 5;
                         }(counter)};
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(counter == 2 * (2 * 3) * (4 * 5));
    REQUIRE(4ms < elapsed);
#ifdef NDEBUG
    REQUIRE(elapsed < 5ms);
#endif
}

TEST_CASE("two voids with sleeps", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<void>> tasks;

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{1ms};
            x += 1;
            co_return;
        }(counter1));

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x += 1;
            co_return;
        }(counter2));

        auto const tmp = co_await WhenAllDyn{std::move(tasks)};

        counter1 += 2;
        REQUIRE(tmp == std::vector{Void<>{}, Void<>{}});
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
    REQUIRE(2ms < elapsed);
    REQUIRE(elapsed < 3ms);
}

TEST_CASE("an int", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<int>> tasks;

        tasks.push_back([]() -> Task<int> {
            co_return 2;
        }());

        auto const tmp = co_await WhenAllDyn{std::move(tasks)};

        counter += 1;
        REQUIRE(tmp == std::vector{2});
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("check tasks execute simultaneously", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter = 2;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<void>> tasks;

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x *= 2;
            co_await Sleep{2ms};
            x *= 4;
        }(counter));

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x *= 3;
            co_await Sleep{2ms};
            x *= 5;
        }(counter));

        co_await WhenAllDyn{std::move(tasks)};
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(counter == 2 * (2 * 3) * (4 * 5));
    REQUIRE(4ms < elapsed);
#ifdef NDEBUG
    REQUIRE(elapsed < 5ms);
#endif
}

TEST_CASE("one int task", "[WhenAny]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAny{[]() -> Task<int> {
            co_return 1;
        }()};
        counter = 2;
        REQUIRE(tmp == std::variant<int>{1});
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one void task", "[WhenAny]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAny{[]() -> Task<void> {
            co_return;
        }()};
        counter = 2;
        REQUIRE(tmp == std::variant<Void<>>{});
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one void task, one int which should be canceled", "[WhenAny]") {
    ThisThreadExecutor executor;
    int counter = 1;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAny{[]() -> Task<void> {
                                              co_return;
                                          }(),
                                          [](auto& counter) -> Task<int> {
                                              co_await Sleep{1ms};
                                              counter += 1;
                                              co_return 1;
                                          }(counter)};
        counter *= 2;
        REQUIRE(tmp == std::variant<Void<>, int>{Void<>{}});
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("any of 5ms and 10ms is 5ms", "[WhenAny]") {
    // ThisThreadExecutor doesn't work here, because it is not time
    // precise. It intentionally doesn't employ conditional variable
    // to optimize for speed.

    ThreadPoolExecutor executor;
    auto const start = steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await WhenAny{[]() -> Task<void> {
                             co_await Sleep{13ms};
                         }(),
                         []() -> Task<void> {
                             co_await Sleep{40ms};
                         }()};
    }());
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(13ms <= elapsed);
    REQUIRE(elapsed <= 14ms);
}

TEST_CASE("one void", "[WhenAnyDyn]") {
    ThisThreadExecutor executor;
    int counter = 1;
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<void>> tasks;
        tasks.push_back([]() -> Task<void> {
            co_return;
        }());
        auto const tmp = co_await WhenAnyDyn{std::move(tasks)};
        counter = 2;
        REQUIRE(tmp == Void<>{});
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one int", "[WhenAnyDyn]") {
    ThisThreadExecutor executor;
    int counter = 1;
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back([]() -> Task<int> {
            co_return 3;
        }());
        auto const tmp = co_await WhenAnyDyn{std::move(tasks)};
        counter = 2;
        REQUIRE(tmp == 3);
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one canceled", "[WhenAnyDyn]") {
    ThisThreadExecutor executor;
    int counter = 1;
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<int>> tasks;

        tasks.push_back([]() -> Task<int> {
            co_await Sleep{1ms};
            co_return 1;
        }());

        tasks.push_back([](auto& counter) -> Task<int> {
            co_await Sleep{2ms};
            counter += 1;
            co_return 2;
        }(counter));

        auto const tmp = co_await WhenAnyDyn{std::move(tasks)};
        counter *= 2;
        REQUIRE(tmp == 1);
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("check reentrant", "[ThreadPoolExecutor::block_on][fuzz]") {
    constexpr size_t s = 10;
    ThreadPoolExecutor executor;
    std::array<int, s> res;
    std::array<std::thread, s> ths;

    for (auto j = 0; j < 100; ++j) {
        for (auto i = 0u; i < s; ++i) {
            ths[i] = std::thread{[&res, i, &executor] {
                res[i] = executor.block_on([&]() -> Task<int> {
                    co_return 1 + 1;
                }());
            }};
        }

        for (auto& x : ths) {
            x.join();
        }

        for (auto x : res) {
            REQUIRE(x == 2);
        }
    }
}

TEST_CASE("additional threads", "[ThreadPoolExecutor::block_on]") {
    using namespace std::chrono_literals;
    ThreadPoolExecutor executor;
    auto const start = steady_clock::now();
    executor.block_on(WhenAll{
                              []() -> Task<void> {
                                  // We don't want all three sleeps to end up
                                  // in one thread's batch. See ft1: Batching.
                                  co_await Sleep{1ms};
                                  std::this_thread::sleep_for(9ms);
                                  co_return;
                              }(),
                              []() -> Task<void> {
                                  co_await Sleep{2ms};
                                  std::this_thread::sleep_for(8ms);
                                  co_return;
                              }(),
                              []() -> Task<void> {
                                  co_await Sleep{3ms};
                                  std::this_thread::sleep_for(7ms);
                                  co_return;
                              }(),
                      },
                      2);
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(elapsed >= 10ms);
    REQUIRE(elapsed <= 13ms);
}

TEST_CASE("check actual 2 threads do the tasks", "[ThreadPoolExecutor::block_on]") {
    using namespace std::chrono_literals;
    ThreadPoolExecutor executor;

    auto const start = steady_clock::now();

    std::thread t1{[&] {
        executor.block_on(Sleep{10ms});
    }};

    std::thread t2{[&] {
        executor.block_on(Sleep{10ms});
    }};

    t1.join();
    t2.join();

    auto const elapsed = steady_clock::now() - start;

    REQUIRE(10ms <= elapsed);
    REQUIRE(elapsed <= 13ms);
}

TEST_CASE("block current thread, other tasks should get progress",
          "[ThreadPoolExecutor::block_on]") {
    using namespace std::chrono_literals;
    ThreadPoolExecutor executor;

#ifdef NDEBUG
    auto const start = steady_clock::now();
#endif

    std::thread t1{[&] {
        executor.block_on(Sleep{17ms});
    }};

    std::thread t2{[&] {
        executor.block_on([]() -> Task<void> {
            auto j1 = []() -> Task<void> {
                std::this_thread::sleep_for(5ms);
                co_return;
            }();

            auto j2 = []() -> Task<void> {
                std::this_thread::sleep_for(20ms);
                co_return;
            }();

            co_await WhenAll{std::move(j1), std::move(j2)};
        }());
    }};

    t1.join();
    t2.join();

#ifdef NDEBUG
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(20ms <= elapsed);
    REQUIRE(elapsed <= 21ms);
#endif
}

TEST_CASE("all block_onS return together", "[ThreadPoolExecutor::block_on]") {
    ThreadPoolExecutor exec;

    std::binary_semaphore s{0};
    std::thread t{[&] {
        exec.block_on([](auto& s) -> Task<void> {
            s.release();
            co_await Sleep{10ms};
        }(s));
    }};
    s.acquire();

    auto const start = steady_clock::now();
    exec.block_on(Yield{});
    auto const elapsed = steady_clock::now() - start;

    t.join();

    REQUIRE(elapsed >= 10ms);
}

TEST_CASE("change executor and yield", "[WithExecutor]") {
    ThisThreadExecutor exec;
    ThreadPoolExecutor pool_exec;

    ConditionVariable cv;

    std::thread t1{[&] {
        pool_exec.block_on([](auto& cv) -> Task<void> {
            cv.notify_one();
            co_await cv.wait();
        }(cv));
    }};

    ThisThreadExecutor{}.block_on(cv.wait()); // ticket-1: use semaphore

    ALONITE_SCOPE_EXIT {
        t1.join();
    };

    exec.block_on([](auto& ex, auto& cv) -> Task<void> {
        co_await WithExecutor{&ex, Yield{}};
        cv.notify_all();
    }(pool_exec, cv));
}

TEST_CASE("change executor and sleep", "[WithExecutor]") {
    ThisThreadExecutor exec;
    ThreadPoolExecutor pool_exec;

    ConditionVariable cv;

    std::thread t1{[&] {
        pool_exec.block_on([](auto& cv) -> Task<void> {
            cv.notify_one();
            co_await cv.wait();
        }(cv));
    }};

    ThisThreadExecutor{}.block_on(cv.wait()); // ticket-1: use semaphore

    ALONITE_SCOPE_EXIT {
        t1.join();
    };

    exec.block_on([](auto& ex, auto& cv) -> Task<void> {
        co_await WithExecutor{&ex, Sleep{10ms}};
        cv.notify_all();
    }(pool_exec, cv));
}

TEST_CASE("two thread sleeps are done in parallel", "[WithExecutor]") {
    ThisThreadExecutor exec;
    ThreadPoolExecutor pool_exec;

    ConditionVariable cv;

    std::thread t1{[&] {
        pool_exec.block_on([](auto& cv) -> Task<void> {
            cv.notify_one();
            co_await cv.wait();
        }(cv));
    }};

    ThisThreadExecutor{}.block_on(cv.wait()); // ticket-1: use semaphore

    std::thread t2{[&] {
        pool_exec.block_on(Yield{});
    }};

    std::thread t3{[&] {
        pool_exec.block_on(Yield{});
    }};

    ALONITE_SCOPE_EXIT {
        t1.join();
        t2.join();
        t3.join();
    };

#ifdef NDEBUG
    auto const start = steady_clock::now();
#endif

    exec.block_on([](auto& ex, auto& cv) -> Task<void> {
        co_await WithExecutor{&ex,
                              WhenAll{[]() -> Task<void> {
                                          // We don't want all three sleeps to end up
                                          // in one thread's batch. See ft1: Batching.
                                          co_await Sleep{1ms};
                                          std::this_thread::sleep_for(99ms);
                                          co_return;
                                      }(),
                                      []() -> Task<void> {
                                          co_await Sleep{2ms};
                                          std::this_thread::sleep_for(98ms);
                                          co_return;
                                      }(),
                                      []() -> Task<void> {
                                          co_await Sleep{3ms};
                                          std::this_thread::sleep_for(97ms);
                                          co_return;
                                      }()}};
        cv.notify_all();
        co_return;
    }(pool_exec, cv));

#ifdef NDEBUG
    auto const elapsed = steady_clock::now() - start;
    REQUIRE(100ms <= elapsed);
    REQUIRE(elapsed < 103ms);
#endif
}

TEST_CASE(
        "a timeout on a different executor on upstream cancels the coroutine and "
        "decrements external work on the correct executor",
        "[WithExecutor]") {
    ThisThreadExecutor exec;

    exec.block_on([&exec]() -> Task<void> {
        ThreadPoolExecutor pool_exec;
        ConditionVariable cv;

        std::thread t1{[&] {
            pool_exec.block_on([](auto& cv) -> Task<void> {
                cv.notify_one();
                co_await cv.wait();
            }(cv));
        }};

        co_await cv.wait(); // ticket-1: use semaphore

        std::thread t2{[&] {
            pool_exec.block_on(Yield{});
        }};

        ALONITE_SCOPE_EXIT {
            t1.join();
            t2.join();
        };

        try {
            co_await Timeout{
                    10ms,
                    WithExecutor{&pool_exec,
                                 /*this adds external work on `pool_exec`; it needs
                                    subtract the external work from `pool_exec`, even
                                    though it is `exec` who destroys the stack*/
                                 WithExecutor{&exec, Sleep{20ms}}}};
        } catch (TimedOut const&) {
        }

        cv.notify_all();
    }());
}
