#pragma once
#include <chrono>
#include <cstdint>
namespace nt::ee {
using NativeClock=std::uint64_t (*)();
inline std::uint64_t native_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
