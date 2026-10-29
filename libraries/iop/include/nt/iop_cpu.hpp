#pragma once
#include <nt/binary.hpp>
#include <nt/iop_devices.hpp>
#include <nt/iop_calls.hpp>
#include <nt/iop_state.hpp>
#include <optional>

namespace nt::iop {
// Bounded diagnostic R3000 integer execution against caller-owned IOP RAM.
// Unimplemented devices, exceptions and coprocessors stop before committing.
CpuResult run(Cpu& cpu, std::span<std::uint8_t> ram, std::uint64_t budget, Devices* devices=nullptr, HostCalls* calls=nullptr);
}
