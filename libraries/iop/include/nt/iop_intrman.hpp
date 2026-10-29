#pragma once
#include <nt/iop_calls.hpp>
#include <nt/iop_devices.hpp>
#include <nt/iop_libraries.hpp>
#include <nt/iop_native_library.hpp>
#include <nt/iop_cpu.hpp>

namespace nt::iop {
struct InterruptHandler {
    std::uint32_t entry, argument, mode;
};
// Partial BIOS-free intrman provider. Storage is supplied by the caller's
// allocator; this class neither claims free guest memory nor installs firmware.
class Intrman final : public HostCalls {
public:
    Intrman(BootstrapDevices& devices,std::uint32_t base);
    void install(std::span<std::uint8_t> ram,Libraries& libraries) { library_.install(ram,libraries); }
    const elf::IopModule& module() const { return library_.module(); }
    bool owns(std::uint32_t pc,std::size_t size) const override { return library_.ordinal(pc,size).has_value(); }
    static constexpr unsigned allocation_size() { return NativeLibrary::allocation_size(32)+2048; }
    std::uint32_t registration_flag() const { return library_.registration_flag(); }
    const std::optional<InterruptHandler>& handler(unsigned irq) const { return handlers_.at(irq); }
    std::uint32_t context_callback() const { return context_callback_; }
    std::uint32_t preemption_callback() const { return preemption_callback_; }
    CallResult invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& arguments,
                      std::span<std::uint8_t> ram,bool interrupt_context) override;
    CallResult syscall(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) override;
    CallResult interrupt(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) override;
private:
    CallResult kernel_call(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram,bool returning);
    struct KernelCall { std::uint32_t stack, return_address, continuation, status; unsigned depth; };
    std::vector<KernelCall> kernel_calls_;
    CallResult interrupt_return(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram,unsigned ordinal);
    CallResult interrupt_schedule(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram);
    CallResult restore_interrupted(const Cpu& cpu,Cpu& next);
    CallResult continue_dma(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram);
    void acknowledge_dma(unsigned channel);
    InterruptController& controller_;
    BootstrapDevices& devices_;
    NativeLibrary library_;
    std::array<std::optional<InterruptHandler>,64> handlers_{};
    std::uint32_t context_callback_=0, preemption_callback_=0;
    bool context_pending_=false;
    std::optional<Cpu> interrupted_;
    unsigned active_irq_=0;
    std::optional<unsigned> active_dma_;
    bool preemption_pending_=false;
};
}
