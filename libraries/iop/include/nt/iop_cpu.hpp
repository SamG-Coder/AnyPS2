#pragma once
#include <nt/binary.hpp>
#include <nt/iop_devices.hpp>
#include <nt/iop_calls.hpp>
#include <array>
#include <optional>

namespace nt::iop {
struct Cpu {
    std::array<std::uint32_t,32> registers{};
    std::uint32_t pc=0, branch_target=0;
    std::uint32_t load_value=0;
    std::uint32_t status=0;
    std::uint32_t cause=0, epc=0;
    std::uint32_t hi=0, lo=0;
    unsigned load_register=0;
    unsigned interrupt_depth=0;
    bool delay_slot=false;
    std::uint64_t executed=0;
};
enum class CpuStop { budget, unsupported, address_fault, branch_in_delay, overflow, interrupt_pending };
struct CpuResult { CpuStop reason; std::uint32_t pc, instruction; std::optional<std::uint32_t> address{}; };
// Bounded diagnostic R3000 integer execution against caller-owned IOP RAM.
// Unimplemented devices, exceptions and coprocessors stop before committing.
CpuResult run(Cpu& cpu, std::span<std::uint8_t> ram, std::uint64_t budget, Devices* devices=nullptr, HostCalls* calls=nullptr);
}
