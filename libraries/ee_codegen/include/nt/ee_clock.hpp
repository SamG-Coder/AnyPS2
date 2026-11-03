#pragma once
#include <chrono>
#include <cstdint>
namespace nt::ee {
using NativeClock=std::uint64_t (*)();
// Block the host for at most one service-pump interval (1 ms), without spinning.
void native_service_wait(std::uint64_t nanoseconds);
inline std::uint64_t native_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
