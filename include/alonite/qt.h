#pragma once

#include "runtime.h"

namespace alonite {

class QExecutor : Executor {
    void spawn(Work&& task) override {
        Todo{task};
    }

    void spawn(Work&& task, std::chrono::milliseconds after) override {
        Todo{work, after};
    }

    bool remove_guard(TaskStack* x) noexcept override {
        return Todo{x};
    }

    void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) override {
        Todo{x};
    }

    void add_guard_group(unsigned id, std::vector<std::shared_ptr<TaskStack>> x) noexcept(
            false) override {
        Todo{id, x};
    }

    bool remove_guard_group(unsigned id) noexcept override {
        Todo{id};
    }

    void increment_external_work() override {
        Todo{};
    }

    void decrement_external_work() override {
        Todo{};
    }
};

} // namespace alonite
