#pragma once

#include <cstdlib>
#include <format>
#include <source_location>
#include <stdexcept>

namespace alonite {

struct Invariant {
    template <class... Ts> // todo: optional<string_view>
    [[noreturn]] void failed(std::source_location const& l, Ts... msg) {
#ifdef alonite_ABORT_ON_INVARIANT_VIOLATION
        static_cast<void>(l);
        (static_cast<void>(msg), ...);
        std::abort();
#else
        if constexpr (sizeof...(msg) > 0) {
            throw std::runtime_error(std::format("invariant failed at {}:{}:{} ({}) - {}",
                                                 l.file_name(),
                                                 l.line(),
                                                 l.column(),
                                                 l.function_name(),
                                                 msg...));
        } else {
            throw std::runtime_error(std::format(
                    "invariant failed at {}:{}:{} ({})", l.file_name(), l.line(), l.column(), l.function_name()));
        }
#endif
    }
};

#define alonite_assert_2_args(expr, module)                                                                            \
    ((expr) || (module.failed(std::source_location::current()), true), AnyExpr{})
#define alonite_assert_3_args(expr, module, msg)                                                                       \
    ((expr) || (module.failed(std::source_location::current(), msg), true), AnyExpr{})

#define get_4rd_arg(arg1, arg2, arg3, arg4, ...) arg4
#define alonite_assert_argument_chooser(...) get_4rd_arg(__VA_ARGS__, alonite_assert_3_args, alonite_assert_2_args, )

/// alonite_assert(expr, module, opitonal<string_view>)
#define alonite_assert(...) alonite_assert_argument_chooser(__VA_ARGS__)(__VA_ARGS__)

struct AnyExpr {
    [[maybe_unused]] AnyExpr() noexcept = default;

    template <class T>
    [[maybe_unused]] [[noreturn]] operator T&() const noexcept {
        std::abort();
    }
};

#define alonite_unreachable(module) (module.failed(std::source_location::current()), AnyExpr{})

} // namespace alonite
