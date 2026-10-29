#pragma once
#include <nt/iop_cpu.hpp>
#include <nt/iop_libraries.hpp>

namespace nt::iop {
enum class StartupStop { budget, returned, fault };
struct StartupResult {
    StartupStop reason;
    std::uint32_t return_value;
    CpuResult execution;
};
struct EntryRegisters { std::array<std::uint32_t,4> arguments{}; };
// RAM and provider modules must outlive the call. Stack ownership/allocation
// belongs to the runtime; this object initializes and executes one entry call.
class Startup {
public:
    Startup(std::span<std::uint8_t> ram, elf::IopModule module, const Libraries& libraries,
            std::uint32_t stack_begin, std::uint32_t stack_end, std::span<const std::string> arguments, Devices* devices=nullptr, HostCalls* calls=nullptr);
    Startup(std::span<std::uint8_t> ram, elf::IopModule module, const Libraries& libraries,
            std::uint32_t stack_begin, std::uint32_t stack_end, EntryRegisters arguments, Devices* devices=nullptr, HostCalls* calls=nullptr);
    // Call a function in an already installed module. The owner supplies bounds
    // and GP only; initialized RAM and bound imports are never overwritten.
    Startup(std::span<std::uint8_t> ram, const elf::IopModule& owner, std::uint32_t entry,
            std::uint32_t stack_begin, std::uint32_t stack_end, EntryRegisters arguments, Devices* devices=nullptr, HostCalls* calls=nullptr);
    StartupResult resume(std::uint64_t budget);
    const Cpu& cpu() const { return cpu_; }
private:
    static constexpr std::uint32_t return_address=0xfffffff0U;
    std::span<std::uint8_t> ram_;
    Cpu cpu_;
    Devices* devices_=nullptr;
    HostCalls* calls_=nullptr;
    std::optional<StartupResult> terminal_;
};
}
