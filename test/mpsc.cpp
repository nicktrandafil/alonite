#include "alonite/mpsc.h"

#include <alonite/scope_exit.h>

#include <catch2/catch_all.hpp>

using namespace alonite;
using std::chrono_literals::operator""ms;

namespace Catch {

template <>
struct StringMaker<std::optional<int>> {
    static std::string convert(std::optional<int> const& value) {
        if (value) {
            return std::to_string(*value);
        } else {
            return std::string("<empty>");
        }
    }
};

template <>
struct StringMaker<std::nullopt_t> {
    static std::string convert(std::nullopt_t const&) {
        return std::string("<empty>");
    }
};

} // namespace Catch

TEST_CASE("construct and send one value", "[mpsc][unbound]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([](auto tx, auto rx, auto& counter) -> Task<void> {
        tx.send(5);
        auto const x = co_await rx.recv();
        ++counter;
        REQUIRE(x == 5);
        co_return;
    }(std::move(tx), std::move(rx), counter));
    REQUIRE(counter == 1);
}

TEST_CASE("construct and send many values, recv doesn't block", "[mpsc][unbound]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([](auto tx, auto rx, auto& counter) -> Task<void> {
        spawn([](auto tx) -> Task<void> {
            for (int i = 0; i < 10; ++i) {
                tx.send(i);
            }
            co_return;
        }(std::move(tx)));

        for (int i = 0; i < 10; ++i) {
            auto const x = co_await rx.recv();
            REQUIRE(x == i);
            ++counter;
        }

        co_return;
    }(std::move(tx), std::move(rx), counter));
    REQUIRE(counter == 10);
}

TEST_CASE("construct and send many values, recv blocks", "[mpsc][unbound]") {
    constexpr int n = 10;
    constexpr auto delay = 5ms;
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThisThreadExecutor executor;
    int counter = 0;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([](auto tx, auto rx, auto& counter, auto delay) -> Task<void> {
        spawn([](auto tx, auto delay) -> Task<void> {
            for (int i = 0; i < n; ++i) {
                co_await Sleep{delay};
                tx.send(i);
            }
        }(std::move(tx), delay));

        for (int i = 0; i < n; ++i) {
            auto const x = co_await rx.recv();
            REQUIRE(x == i);
            ++counter;
        }

        co_return;
    }(std::move(tx), std::move(rx), counter, delay));
    REQUIRE(counter == n);
    auto const dur = std::chrono::steady_clock::now() - start;
    REQUIRE(delay * n <= dur);
    REQUIRE(dur <= delay * n + delay);
}

TEST_CASE("send values for 500ms; sender is dropped", "[mpsc][unbound]") {
    using namespace std::chrono_literals;
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThreadPoolExecutor executor;
    int n = 0;
    executor.block_on([](auto tx, auto rx, auto& n) -> Task<void> {
        spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                          for (int i = 0; true; ++i) {
                              tx.send(i);
                              co_await Yield{};
                          }
                          co_return;
                      }(std::move(tx))});

        while (auto const x = co_await rx.recv()) {
            REQUIRE(x == n++);
        }

        co_return;
    }(std::move(tx), std::move(rx), n));
    REQUIRE(n > 150000);
}

TEST_CASE("send values for 500ms concurrently", "[mpsc][unbound]") {
    using namespace std::chrono_literals;
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThreadPoolExecutor executor;
    int n = 0;
    executor.block_on(
            [](auto tx, auto rx, auto& n) -> Task<void> {
                spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                                  for (int i = 0; true; ++i) {
                                      tx.send(i);
                                      co_await Yield{};
                                  }
                                  co_return;
                              }(std::move(tx))});

                spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                                  for (int i = 0; true; ++i) {
                                      tx.send(i);
                                      co_await Yield{};
                                  }
                                  co_return;
                              }(std::move(tx))});

                spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                                  for (int i = 0; true; ++i) {
                                      tx.send(i);
                                      co_await Yield{};
                                  }
                                  co_return;
                              }(std::move(tx))});

                while (auto const x = co_await rx.recv()) {
                    REQUIRE(x == n++);
                }

                co_return;
            }(std::move(tx), std::move(rx), n),
            3);
    REQUIRE(n > 100000);
}

TEST_CASE("auto-close on receiver drop", "[mpsc][unbound]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    [](auto) {
    }(std::move(rx));
    REQUIRE_THROWS_AS(tx.send(1), mpsc::ClosedError);
}

TEST_CASE("auto-close on all senders drop", "[mpsc][unbound]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    [](auto) {
    }(std::move(tx));
    ThisThreadExecutor{}.block_on([](auto rx) -> Task<void> {
        REQUIRE(co_await rx.recv() == std::nullopt);
    }(std::move(rx)));
}

TEST_CASE("you can receive buffered messages from a closed channel", "[mpsc][unbound]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    tx.send(1);
    [](auto) {
    }(std::move(tx));
    ThisThreadExecutor{}.block_on([](auto rx) -> Task<void> {
        auto const x = co_await rx.recv();
        REQUIRE(x == 1);
        REQUIRE(co_await rx.recv() == std::nullopt);
    }(std::move(rx)));
}

TEST_CASE("construct and send one value", "[mpsc][bound]") {
    auto [tx, rx] = mpsc::channel<int>(1);
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([](auto tx, auto rx, auto& counter) -> Task<void> {
        co_await tx.send(5);
        auto const x = co_await rx.recv();
        ++counter;
        REQUIRE(x == 5);
        co_return;
    }(std::move(tx), std::move(rx), counter));
    REQUIRE(counter == 1);
}

TEST_CASE("construct and send many values, send and recv block intermittently", "[mpsc][bound]") {
    auto [tx, rx] = mpsc::channel<int>(1);
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([](auto tx, auto rx, auto& counter) -> Task<void> {
        spawn([](auto tx) -> Task<void> {
            for (int i = 0; i < 10; ++i) {
                co_await tx.send(i);
            }
            co_return;
        }(std::move(tx)));

        for (int i = 0; i < 10; ++i) {
            auto const x = co_await rx.recv();
            REQUIRE(x == i);
            ++counter;
        }

        co_return;
    }(std::move(tx), std::move(rx), counter));
    REQUIRE(counter == 10);
}

TEST_CASE("receiver is dropped while the sender is suspended", "[mpsc][bound]") {
    auto [tx, rx] = mpsc::channel<int>(1);
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([](auto tx, auto rx, auto& counter) -> Task<void> {
        auto task = spawn([](auto tx) -> Task<void> {
            co_await tx.send(0);
            co_await tx.send(1);
        }(std::move(tx)));

        co_await Sleep{1ms};
        [](auto) {
        }(std::move(rx));

        try {
            co_await task;
        } catch (...) {
        }

        counter++;

        co_return;
    }(std::move(tx), std::move(rx), counter));
    REQUIRE(counter == 1);
}

TEST_CASE("send values for 500ms; sender is dropped", "[mpsc][bound]") {
    using namespace std::chrono_literals;
    auto [tx, rx] = mpsc::channel<int>(10);
    ThreadPoolExecutor executor;
    int n = 0;
    executor.block_on([](auto tx, auto rx, auto& n) -> Task<void> {
        spawn(Timeout{500ms, [](auto tx) -> Task<void> {
            for (int i = 0; true; ++i) {
                co_await tx.send(i);
                co_await Yield{};
            }
            co_return;
        }(std::move(tx))});

        while (auto const x = co_await rx.recv()) {
            REQUIRE(x == n++);
        }

        co_return;
    }(std::move(tx), std::move(rx), n));
    REQUIRE(n > 150000);
}

TEST_CASE("send values for 500ms concurrently", "[mpsc][bound]") {
    using namespace std::chrono_literals;
    auto [tx, rx] = mpsc::channel<int>(3);
    ThreadPoolExecutor executor;
    int n = 0;
    executor.block_on(
        [](auto tx, auto rx, auto& n) -> Task<void> {
            spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                for (int i = 0; true; ++i) {
                    co_await tx.send(i);
                    co_await Yield{};
                }
                co_return;
            }(std::move(tx))});

            spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                for (int i = 0; true; ++i) {
                    co_await tx.send(i);
                    co_await Yield{};
                }
                co_return;
            }(std::move(tx))});

            spawn(Timeout{500ms, [](auto tx) -> Task<void> {
                for (int i = 0; true; ++i) {
                    co_await tx.send(i);
                    co_await Yield{};
                }
                co_return;
            }(std::move(tx))});

            while (auto const x = co_await rx.recv()) {
                REQUIRE(x == n++);
            }

            co_return;
        }(std::move(tx), std::move(rx), n),
                      3);
    REQUIRE(n > 100000);
}

TEST_CASE("you can receive buffered messages from a closed channel", "[mpsc][bound]") {
    auto [tx, rx] = mpsc::channel<int>(1);
    ThisThreadExecutor{}.block_on([](auto tx, auto rx) -> Task<void> {
        co_await tx.send(1);
        [](auto) {
        }(std::move(tx));
        auto const x = co_await rx.recv();
        REQUIRE(x == 1);
        REQUIRE(co_await rx.recv() == std::nullopt);
    }(std::move(tx), std::move(rx)));
}
