#pragma once
#include <nt/iop_state.hpp>
#include <nt/iop_memory.hpp>
#include <span>

namespace nt::iop {
// Values are also the generated LLVM function's C ABI return codes.
enum class NativeStop : std::uint32_t { budget=0, unmapped_code=1, address_fault=2, branch_in_delay=3, code_write=4, guest_break=5, service_request=6, overflow=8, import_request=9, thread_yield=10, thread_wait=11, serial_request=12 };
struct NativeResult { NativeStop reason; std::uint32_t pc, address=0, trap_code=0; };
using NativeEntry=NativeResult (*)(Cpu&,std::span<std::uint8_t>,std::uint64_t);
enum class NativeModuleKind { compiled, archive };
struct NativeModule { std::uint32_t base, entry, initialized_size, memory_size, global_pointer; NativeModuleKind kind=NativeModuleKind::compiled; };
// These are data-memory helpers only. Generated programs have no code fetch,
// instruction decoder, device interpreter or fallback execution entry point.
inline std::uint32_t load_native_word(std::span<const std::uint8_t> ram,std::uint32_t offset) {
    std::uint32_t value=0;
    for (unsigned byte=0;byte<4;++byte) value|=std::uint32_t(ram[offset+byte])<<(byte*8);
    return value;
}
inline void store_native_word(std::span<std::uint8_t> ram,std::uint32_t offset,std::uint32_t value) {
    for (unsigned byte=0;byte<4;++byte) ram[offset+byte]=static_cast<std::uint8_t>(value>>(byte*8));
}
}
