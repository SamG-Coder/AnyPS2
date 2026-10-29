#include <nt/iop_intrman.hpp>
#include <nt/iop_memory.hpp>

namespace nt::iop {
CallResult Intrman::kernel_call(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram,bool returning) {
    if (cpu.delay_slot) return {};
    if (returning) {
        if (kernel_calls_.empty()) return {};
        const auto saved=kernel_calls_.back();
        if (!ram_offset(saved.continuation,4,ram.size()))
            return {CallState::address_fault,0,saved.continuation};
        next=cpu;
        if (cpu.load_register) next.registers[cpu.load_register]=cpu.load_value;
        next.pc=saved.continuation; next.registers[29]=saved.stack;
        next.registers[31]=saved.return_address; next.status=saved.status;
        next.interrupt_depth=saved.depth; next.load_register=0; next.delay_slot=false;
        kernel_calls_.pop_back();
        return {CallState::transferred};
    }
    if ((cpu.status&0x400000) || kernel_calls_.size()>=32) return {};
    auto entered=cpu;
    if (cpu.load_register) entered.registers[cpu.load_register]=cpu.load_value;
    const auto target=entered.registers[4], stack=entered.registers[29];
    if (!target || !ram_offset(target,4,ram.size())) return {CallState::address_fault,0,target};
    constexpr unsigned reserve=28;
    if (stack<reserve) return {CallState::address_fault,0,stack};
    const auto offset=ram_offset(stack-reserve,4,ram.size());
    if (!offset || reserve>ram.size()-*offset) return {CallState::address_fault,0,stack-reserve};
    // Keep callback execution on the normal CPU path. The bounded host record
    // owns only the wrapper continuation; guest register and memory effects survive.
    const auto pushed=(cpu.status&~0x3fU)|((cpu.status&15)<<2);
    const auto restored=(pushed&~15U)|((pushed>>2)&15);
    kernel_calls_.push_back({stack,entered.registers[31],cpu.pc+4,restored,cpu.interrupt_depth});
    next=entered;
    next.registers[4]=entered.registers[5]; next.registers[5]=entered.registers[6]; next.registers[6]=entered.registers[7];
    next.registers[29]=stack-reserve; next.registers[31]=library_.module().entry+36;
    next.pc=target; next.status=pushed; next.epc=cpu.pc;
    next.cause=(cpu.cause&~0x8000007cU)|0x20;
    next.load_register=0; next.delay_slot=false;
    return {CallState::transferred};
}
}
