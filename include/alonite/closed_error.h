#pragma once

#include <any>
#include <exception>
#include <optional>

namespace alonite {

struct ClosedError : std::exception {
    explicit ClosedError(std::optional<std::any> value = {})
            : value{std::move(value)} {
    }

    const char* what() const noexcept override {
        return "ClosedError";
    }

    /// If suspending send failed, then the value reported back here.
    std::optional<std::any> value;
};

} // namespace alonite
