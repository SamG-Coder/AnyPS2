#pragma once
#include <cstdint>
namespace nt::ee {
constexpr bool startup_syscall(std::uint32_t number) {
    return number==0x3c || number==0x3d || number==0x3e;
}
}
