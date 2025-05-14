#pragma once

#include "common.h"
#include "mpsc.h"

namespace alonite {

class Mutex {
public:
    Mutex()
            : channel{mpsc::channel<void>(1)} {
        channel.value().first.try_send();
    }

    Mutex(Mutex const&) = delete;

    Mutex& operator=(Mutex const&) = delete;

    Mutex(Mutex&& rhs)
            : channel{take(rhs.channel)} {
    }

    Mutex& operator=(Mutex&& rhs) {
        channel = take(rhs.channel);
        return *this;
    }

    Task<void> lock() {
        co_await channel.value().second.recv();
    }

    void unlock() {
        channel.value().first.try_send();
    }

private:
    std::optional<std::pair<mpsc::Sender<void>, mpsc::Receiver<void>>> channel;
};

} // namespace alonite
