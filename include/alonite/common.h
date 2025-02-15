#pragma once

#include <optional>

namespace alonite {

template <class T>
std::optional<T> take(std::optional<T>& x) {
    std::optional<T> ret;
    ret.swap(x);
    return ret;
}

template <class T>
T* take(T*& x) {
    T* ret{nullptr};
    std::swap(ret, x);
    return ret;
}

}
