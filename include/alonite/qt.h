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

    void spawn(Work&& w, std::chrono::steady_clock::time_point at) override {
        QTimer::singleShot(at - std::chrono::steady_clock::now(), std::move(w));
    }
};

struct Disconnected : Error {
    char const* what() const noexcept override {
        return "Disconnected";
    }
};

// Connect a signal to a channel
template <class T, class Sink, class... Args>
    requires (sizeof...(Args) > 1 && requires (Sink s, Args const&... args) { { s.send(std::tuple{args...}) }; })
          || (sizeof...(Args) <= 1 && requires(Sink s, Args const&... args) { { s.send(args...) }; })
auto connect(T& obj, void (T::*sig)(Args...), Sink channel) {
    if constexpr (sizeof...(Args) > 1) {
        return QObject::connect(
                &obj, sig, [channel = std::move(channel)](Args const&... args) {
                    channel.send(std::tuple(args...));
                });
    } else {
        return QObject::connect(
                &obj, sig, [channel = std::move(channel)](Args const&... args) {
                    channel.send(args...);
                });
    }
}

} // namespace alonite
