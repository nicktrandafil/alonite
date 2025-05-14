#include "alonite/mutex.h"

#include "alonite/runtime.h"

#include <catch2/catch_all.hpp>

#include <ranges>

using namespace alonite;
using namespace std;

TEST_CASE(
        "It is important for cancelation functions, like Timeout and WhenAny to wait until the cancelling coroutine is "
        "finished, because it might refer to something from outer scope.",
        "[fuzz]") {
    ThreadPoolExecutor{}.block_on(
            []() -> Task<void> {
                std::string state;
                Mutex m;

                std::vector<Task<void>> tasks;
                for (auto const i : views::iota(size_t{0}, thread::hardware_concurrency())) {
                    tasks.push_back([](auto& m, auto& state, auto id) -> Task<void> {
                        while (true) {
                            co_await m.lock();
                            state += id;
                            m.unlock();
                        }
                    }(m, state, "|" + to_string(i)));
                }

                co_await Timeout{chrono::milliseconds(100), WhenAllDyn{std::move(tasks)}};

                INFO("state = " << state);

                for (auto const i : views::iota(size_t{0}, thread::hardware_concurrency())) {
                    REQUIRE(state.find("|" + to_string(i)) != string::npos);
                }
            }(),
            thread::hardware_concurrency());
}
