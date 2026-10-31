#pragma once
#include <cstdint>
namespace nt::ee {
constexpr bool cache_syscall(std::uint32_t number) { return number==0x64 || number==0xffffff98U; }
constexpr bool intc_syscall(std::uint32_t number) {
    return number==0x12 || number==0x13 || number==0x16 || number==0x17 || number==0xffffffe4U || number==0xffffffe3U || number==0x10 || number==0x11 || number==0x14 || number==0x15 || number==0xffffffe6U || number==0xffffffe5U;
}
constexpr bool thread_syscall(std::uint32_t number) {
    return (number>=0x20 && number<=0x24) || number==0x29 || number==0x2f || (number>=0x30 && number<=0x36);
}
constexpr bool settings_syscall(std::uint32_t number) { return number==0x4a || number==0x4b || number==0x6e || number==0x6f; }
constexpr bool sif_control_syscall(std::uint32_t number) { return number==0x6b || number==0x78 || number==0x79 || number==0x7a; }
constexpr std::uint32_t native_syscall_number(std::uint32_t number) {
    switch (number) {
    case 0xffffffd1U: // Interrupt-context GetThreadId.
    case 0xffffffccU: case 0xffffffcaU: // Interrupt-safe thread wake/cancel.
    case 0xffffff90U: case 0xffffff8fU: // Interrupt-safe GS mask access.
    case 0xffffffbdU: case 0xffffffbaU: case 0xffffffb8U: // Interrupt-safe semaphore services.
    case 0xffffff8aU: case 0xffffff89U: case 0xffffff88U: // Interrupt-safe SIF services.
        return 0U-number;
    default: return number;
    }
}
constexpr bool startup_syscall(std::uint32_t number) {
    number=native_syscall_number(number);
    return number==2 || number==0x70 || number==0x71 || number==0x76 || number==0x77 || sif_control_syscall(number) || settings_syscall(number) || thread_syscall(number) || intc_syscall(number) || cache_syscall(number) || number==0x74 || number==0x3c || number==0x3d || number==0x3e || (number>=0x40 && number<=0x48);
}
}
