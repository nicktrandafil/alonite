#include <alonite/mpsc.h>
#include <alonite/runtime.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

static unsigned global = 0;

static alonite::Task<void> my_coro_when_all_10k() {
    std::vector<alonite::JoinHandle<void>> tasks;
    tasks.reserve(10'000);
    for (unsigned i = 0; i < 10'000; ++i) {
        tasks.push_back(alonite::spawn([](unsigned i) -> alonite::Task<void> {
            co_await alonite::Sleep{1ms};
            global += i;
            co_return;
        }(i)));
    }
    co_await alonite::WhenAllDyn{std::move(tasks)};
}

static boost::asio::awaitable<void> boost_when_all_10k() {
    auto const ex = co_await boost::asio::this_coro::executor;

    auto const work = [](unsigned i) -> boost::asio::awaitable<void> {
        co_await boost::asio::steady_timer{co_await boost::asio::this_coro::executor, 1ms}
                .async_wait(boost::asio::use_awaitable);
        global += i;
        co_return;
    };

    std::vector<decltype(boost::asio::co_spawn(ex, work(0), boost::asio::deferred))>
            tasks;
    tasks.reserve(10'000);
    for (unsigned i = 0; i < 10'000; ++i) {
        tasks.push_back(boost::asio::co_spawn(ex, work(i), boost::asio::deferred));
    }

    co_await boost::asio::experimental::make_parallel_group(std::move(tasks))
            .async_wait(boost::asio::experimental::wait_for_all(),
                        boost::asio::use_awaitable);
}

static boost::asio::awaitable<void> boost_spawn_10k() {
    for (unsigned i = 0; i < 10'000; ++i) {
        boost::asio::co_spawn(
                co_await boost::asio::this_coro::executor,
                [i]() -> boost::asio::awaitable<void> {
                    co_await boost::asio::steady_timer{
                            co_await boost::asio::this_coro::executor, 1ms}
                            .async_wait(boost::asio::use_awaitable);
                    global += i;
                },
                boost::asio::detached);
    }
}

static alonite::Task<void> my_co_main3() {
    for (unsigned i = 0; i < 10'000; ++i) {
        alonite::spawn([](unsigned i) -> alonite::Task<void> {
            co_await alonite::Sleep{1ms};
            global += i;
            co_return;
        }(i));
    }
    co_return;
}

static alonite::Task<size_t> my_coro_work_3s() {
    using namespace alonite;

    auto const work = [](size_t) -> Task<void> {
        co_await Sleep{0ms};
    };

    size_t i = 0;

    co_await WhenAny{[&]() -> Task<void> {
                         co_await Sleep{3s};
                     }(),
                     [&]() -> Task<void> {
                         for (; i < 100'000'000; ++i) {
                             co_await work(i);
                         }
                     }()};

    co_return i;
}

// clang-format off
static boost::asio::awaitable<size_t> boost_coro_work_3s() {
    using namespace boost::asio;
    using namespace boost::asio::experimental::awaitable_operators;

    auto const work = [](size_t) -> awaitable<void> {
        co_await steady_timer{co_await this_coro::executor, 0ms}.async_wait(use_awaitable);
    };

    size_t i = 0;

    co_await (
                 steady_timer{co_await this_coro::executor, 3s}.async_wait(use_awaitable)
              || [&]() -> awaitable<void> {
                     for (; i < 100'000'000; ++i) {
                         co_await work(i);
                     }
                 }()
             );

    co_return i;
}
// clang-format on

static boost::asio::awaitable<std::pair<size_t, size_t>> channel_throughput() {
    using namespace boost::asio;
    using namespace boost::asio::experimental::awaitable_operators;
    using namespace boost::asio::experimental;
    using boost::system::error_code;

    channel<void(error_code, int)> channel{co_await this_coro::executor, 1};

    size_t send = 0;
    size_t recv = 0;

    auto const producer = [&]() -> awaitable<void> {
        for (size_t i = 0; i < 100'000'000; ++i) {
            co_await channel.async_send(error_code{}, 0, use_awaitable);
            ++send;
        }
    };

    auto const consumer = [&]() -> awaitable<void> {
        for (size_t i = 0; i < 10'000'000; ++i) {
            co_await channel.async_receive(use_awaitable);
            ++recv;
        }
    };

    co_await (
            (producer() && consumer())
            || steady_timer{co_await this_coro::executor, 3s}.async_wait(use_awaitable));

    co_return std::pair{send, recv};
}

static alonite::Task<std::pair<size_t, size_t>> my_coro_channel_throughput() {
    using namespace alonite;

    auto [tx, rx] = mpsc::unbound_channel<int>();

    size_t send = 0;
    size_t recv = 0;

    auto const producer = [&]() -> Task<void> {
        for (size_t i = 0; i < 100'000'000; ++i) {
            tx.send(0);
            ++send;
            co_await Yield{};
        }
    };

    auto const consumer = [&]() -> Task<void> {
        for (size_t i = 0; i < 10'000'000; ++i) {
            co_await rx.recv();
            ++recv;
        }
    };

    co_await WhenAny{[&]() -> Task<void> {
                         co_await WhenAll{producer(), consumer()};
                     }(),
                     []() -> Task<void> {
                         co_await Sleep{3s};
                     }()};

    co_return std::pair{send, recv};
}

static boost::asio::awaitable<size_t> boost_coro_runtime_overhead() {
    using namespace boost::asio;
    using namespace boost::asio::experimental::awaitable_operators;

    size_t yields = 0;

    auto const work = [&]() -> awaitable<void> {
        for (size_t i = 0; i < 100'000'000; ++i) {
            ++yields;
            co_await boost::asio::post(co_await boost::asio::this_coro::executor,
                                       boost::asio::use_awaitable);
        }
    };

    co_await ([&]() -> awaitable<void> {
        co_await work();
    }() || steady_timer{co_await this_coro::executor, 3s}.async_wait(use_awaitable));

    co_return yields;
}

static alonite::Task<size_t> my_coro_runtime_overhead() {
    using namespace alonite;

    size_t yields = 0;

    auto const work = [&]() -> Task<void> {
        for (size_t i = 0; i < 100'000'000; ++i) {
            ++yields;
            co_await Yield{};
        }
    };

    co_await WhenAny{[&]() -> Task<void> {
                         co_await work();
                     }(),
                     []() -> Task<void> {
                         co_await Sleep{3s};
                     }()};

    co_return yields;
}

static alonite::Task<long> my_coro_fib(int n) {
    if (n < 2) {
        co_return n;
    }

    auto a = my_coro_fib(n - 1);
    auto b = my_coro_fib(n - 2);

    auto [x, y] = co_await alonite::WhenAll(std::move(a), std::move(b));

    co_return x + y;
}

static boost::asio::awaitable<long> boost_coro_fib(int n) {
    using namespace boost::asio;
    using namespace boost::asio::experimental::awaitable_operators;

    if (n < 2) {
        co_return n;
    }

    auto [x, y] = co_await (boost_coro_fib(n - 1) && boost_coro_fib(n - 2));

    co_return x + y;
}

int main() {
    {
        std::cout << "my coro - when all 10k\n";
        alonite::ThisThreadExecutor exec;

        auto const start = std::chrono::steady_clock::now();
        exec.block_on(my_coro_when_all_10k());
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    global = 0;

    {
        std::cout << "boost coro - when all 10k\n";
        boost::asio::io_context io;

        auto const start = std::chrono::steady_clock::now();
        boost::asio::co_spawn(io, boost_when_all_10k(), boost::asio::detached);
        io.run();
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    global = 0;

    {
        std::cout << "my coro - spawn 10k\n";
        alonite::ThisThreadExecutor exec;

        auto const start = std::chrono::steady_clock::now();
        exec.block_on(my_co_main3());
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    global = 0;

    {
        std::cout << "boost coro - spawn 10k\n";
        boost::asio::io_context io;

        auto const start = std::chrono::steady_clock::now();
        boost::asio::co_spawn(io, boost_spawn_10k(), boost::asio::detached);
        io.run();
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "my coro - work/sleep throughput\n";

        using namespace std::chrono_literals;
        using namespace alonite;
        ThreadPoolExecutor exec;

        auto const start = std::chrono::steady_clock::now();

        size_t i = 0;

        // start working
        std::thread t1{[&] {
            i = exec.block_on(my_coro_work_3s());
        }};

        t1.join();

        auto const end = std::chrono::steady_clock::now();

        std::cout << i / 3 << " works in 1s\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "boost coro - work/sleep throughput\n";

        using namespace std::chrono_literals;
        using namespace boost::asio;

        io_context io;

        auto const start = std::chrono::steady_clock::now();

        size_t i = 0;

        // start working
        boost::asio::co_spawn(
                io,
                [&i]() -> boost::asio::awaitable<void> {
                    i = co_await boost_coro_work_3s();
                },
                boost::asio::detached);

        io.run();

        auto const end = std::chrono::steady_clock::now();

        std::cout << i / 3 << " works in 1s\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "boost coro - channel throughput\n";

        using namespace std::chrono_literals;
        using namespace boost::asio;

        io_context io;

        auto const start = std::chrono::steady_clock::now();

        size_t send = 0;
        size_t recv = 0;

        boost::asio::co_spawn(
                io,
                [&]() -> boost::asio::awaitable<void> {
                    std::tie(send, recv) = co_await channel_throughput();
                },
                boost::asio::detached);

        io.run();

        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "send throughput: " << send / 3 << " msg/s\n";
        std::cout << "recv throughput: " << recv / 3 << " msg/s\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "my coro - channel throughput\n";

        using namespace std::chrono_literals;
        using namespace alonite;

        ThreadPoolExecutor exec;

        auto const start = std::chrono::steady_clock::now();

        auto const [send, recv] = exec.block_on(my_coro_channel_throughput());

        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "send throughput: " << send / 3 << " msg/s\n";
        std::cout << "recv throughput: " << recv / 3 << " msg/s\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "boost coro - runtime overhead\n";

        using namespace std::chrono_literals;
        using namespace boost::asio;

        io_context io;

        auto const start = std::chrono::steady_clock::now();

        size_t yields = 0;

        boost::asio::co_spawn(
                io,
                [&yields]() -> boost::asio::awaitable<void> {
                    yields = co_await boost_coro_runtime_overhead();
                },
                boost::asio::detached);

        io.run();

        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "yield throughput: " << yields / 3 << " yields/s\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "my coro - runtime overhead\n";

        using namespace std::chrono_literals;
        using namespace alonite;

        ThreadPoolExecutor exec;

        auto const start = std::chrono::steady_clock::now();

        auto const yields = exec.block_on(my_coro_runtime_overhead());

        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "yield throughput: " << yields / 3 << " yields/s\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "my coro - fib(30)\n";

        using namespace std::chrono_literals;
        using namespace alonite;

        ThreadPoolExecutor exec;

        auto const start = std::chrono::steady_clock::now();

        auto const fib = exec.block_on(my_coro_fib(30));

        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "fib(30)=" << fib << "\n";
    }

    std::cout << "------------------\n";

    {
        std::cout << "boost coro - fib(30)\n";

        using namespace std::chrono_literals;
        using namespace boost::asio;
        using namespace boost::asio::experimental;

        io_context io;

        long x;
        auto const start = std::chrono::steady_clock::now();
        boost::asio::co_spawn(
                io,
                [&]() -> boost::asio::awaitable<void> {
                    x = co_await boost_coro_fib(30);
                },
                boost::asio::detached);

        io.run();
        auto const end = std::chrono::steady_clock::now();

        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";

        std::cout << "fib(30)=" << x << "\n";
    }
}
