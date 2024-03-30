#include <rpc/runtime.h>

#include <catch2/catch_all.hpp>

using namespace rpc;
using namespace std::chrono_literals;

TEST_CASE("value result", "[ThisThreadExecutor::block_on(Task<T>)]") {
    ThisThreadExecutor executor;
    auto const x = executor.block_on([&]() -> Task<int> {
        co_return 1 + 1;
    }());
    REQUIRE(x == 2);
}

TEST_CASE("exception result", "[ThisThreadExecutor::block_on(Task<T>)]") {
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

TEST_CASE("void result", "[ThisThreadExecutor::block_on(Task<T>)]") {
    ThisThreadExecutor executor;
    bool executed = false;
    executor.block_on([&]() -> Task<void> {
        RPC_SCOPE_EXIT {
            executed = true;
        };
        co_return;
    }());
    REQUIRE(executed);
}

TEST_CASE("destruction order should be natural",
          "[ThisThreadExecutor::block_on(Task<T>)]") {
    ThisThreadExecutor executor;

    int acc = 0;

    struct Add {
        [[maybe_unused]] ~Add() noexcept {
            acc *= 2;
            acc += x;
        }

        int& acc;
        int x;
    };

    executor.block_on([&](std::shared_ptr<Add>) -> Task<void> {
        co_await [&](std::shared_ptr<Add>) -> Task<void> {
            co_return;
        }(std::shared_ptr<Add>(new Add{acc, 2}));
    }(std::shared_ptr<Add>(new Add{acc, 1})));

    REQUIRE(acc == 5);
}

TEST_CASE("await for result", "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    executor.block_on([&]() -> Task<void> {
        auto x = co_await executor.spawn([]() -> Task<int> {
            co_return 1 + 1;
        }());
        REQUIRE(x == 2);
        co_return;
    }());
}

TEST_CASE("ignore result", "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([&]() -> Task<int> {
            run = true;
            co_return 1 + 1;
        }());
        co_return;
    }());
    REQUIRE(run);
}

TEST_CASE("use some sleep to actually enter the block_on loop",
          "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([](bool& run) -> Task<int> {
            co_await Sleep{5ms};
            run = true;
            co_return 1 + 1;
        }(run));
        co_return;
    }());
    REQUIRE(run);
}

// TEST_CASE("abort", "[ThisThreadExecutor::spawn(Task<T>)]") {
//     ThisThreadExecutor executor;
//     bool run = false;
//     bool exception = false;
//     executor.block_on([&]() -> Task<void> {
//         auto handle = executor.spawn([&]() -> Task<void> {
//             co_await Sleep{5ms};
//             run = true;
//             co_return;
//         }());

//         co_await Sleep{1ms};
//         handle.abort();

//         try {
//             co_await handle;
//         } catch (Canceled const& e) {
//             exception = true;
//         }

//         co_return;
//     }());
//     REQUIRE(!run);
//     REQUIRE(exception);
// }

TEST_CASE("ignore result", "[Sleep]") {
    ThisThreadExecutor executor;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await Sleep{5ms};
        co_return;
    }());
    auto const end = std::chrono::steady_clock::now();
    REQUIRE(5ms <= end - start);
    REQUIRE(end - start < 10ms);
}

// TEST_CASE("Discard the handle", "[ThisThreadExecutor::spawn(task)]") {
//     ThisThreadExecutor executor;
//     int effect = 0;
//     executor.block_on([&]() -> Task<void> {
//         executor.spawn([](int* effect) -> Task<void> {
//             *effect = 1;
//             co_return;
//         }(&effect));
//         co_return;
//     }());
//     REQUIRE(effect == 1);
// }

// TEST_CASE("Abort", "[ThisThreadExecutor::spawn(task)]") {
//     ThisThreadExecutor executor;
//     int effect = 0;
//     executor.block_on([&]() -> Task<void> {
//         auto h = executor.spawn([](int* effect) -> Task<void> {
//             co_await Sleep{2ms};
//             *effect = 1;
//             co_return;
//         }(&effect));
//         co_await Sleep{1ms};
//         h.abort();
//         co_await h;
//         co_return;
//     }());
//     REQUIRE(effect == 1);
// }
