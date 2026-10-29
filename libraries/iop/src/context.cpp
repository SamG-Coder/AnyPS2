#include <nt/iop_intrman.hpp>
#include <nt/iop_cpu.hpp>
#include <nt/iop_memory.hpp>

namespace nt::iop {
CallResult CallRouter::syscall(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) {
    // An unimplemented native entry must not be reinterpreted as a guest ABI call.
    for (auto* provider : providers_)
        if (provider->owns(cpu.pc,ram.size())) return provider->syscall(cpu,next,ram);
    for (auto* provider : providers_) {
        const auto result=provider->syscall(cpu,next,ram);
        if (result.state!=CallState::unsupported) return result;
    }
    return {};
}
namespace {
constexpr unsigned frame_size=0x98;
constexpr std::uint32_t voluntary=0xf0ff000c, full=0xfffffffe;
std::optional<std::uint32_t> frame_offset(std::uint32_t address,std::size_t size) {
    const auto offset=ram_offset(address,4,size);
    if (!offset || frame_size>size-*offset) return {};
    return offset;
}
void store(std::span<std::uint8_t> ram,unsigned at,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) ram[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
}
CallResult Intrman::syscall(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) {
    const auto ordinal=library_.ordinal(cpu.pc,ram.size());
    if (ordinal) {
        if (*ordinal==14 || *ordinal==3) return kernel_call(cpu,next,ram,*ordinal==3);
        if (*ordinal==1 || *ordinal==2) return interrupt_return(cpu,next,ram,*ordinal);
        // Ordinal zero is an internal return gate only while our callback runs.
        if (*ordinal!=0 || !context_pending_) return {};
        const auto pointer=cpu.registers[2];
        const auto frame=frame_offset(pointer,ram.size());
        if (!frame) return {CallState::address_fault,0,pointer};
        const auto word=[&](unsigned at) { return le32(ram,*frame+at); };
        const auto kind=word(0), status=word(0x88), target=word(0x8c), control=word(0x90);
        if (kind!=voluntary && kind!=full) return {};
        const auto restored=(status&~15U)|((status>>2)&15);
        if ((restored&0x00020002U) || control>1) return {};
        if (!ram_offset(target,4,ram.size())) return {CallState::address_fault,0,target};
        next=cpu;
        for (unsigned reg=1;reg<32;++reg) {
            const bool saved=kind==full ? (reg!=26 && reg!=27) :
                (reg==2 || reg==3 || (reg>=16 && reg<=23) || reg>=28);
            if (saved) next.registers[reg]=word(reg*4);
        }
        if (kind==full) { next.hi=word(0x80); next.lo=word(0x84); }
        next.pc=target; next.status=restored;
        next.delay_slot=false; next.load_register=0; next.interrupt_depth=0;
        controller_.write(0x1f801078,4,control);
        context_pending_=false;
        interrupted_.reset();
        preemption_pending_=false;
        return {CallState::transferred};
    }
    if (cpu.registers[2]==0x0c) return kernel_call(cpu,next,ram,false);
    if (cpu.registers[2]!=0x20 || cpu.delay_slot || cpu.interrupt_depth || context_pending_ || !kernel_calls_.empty() ||
        !context_callback_ || (cpu.status&0x400000) || cpu.registers[6]>1) return {};
    const auto stack=cpu.registers[29];
    if (stack<frame_size+16) return {CallState::address_fault,0,stack};
    const auto pointer=stack-frame_size;
    const auto frame=frame_offset(pointer,ram.size());
    if (!frame || !ram_offset(pointer-16,4,ram.size())) return {CallState::address_fault,0,pointer};
    if (!ram_offset(context_callback_,4,ram.size())) return {CallState::address_fault,0,context_callback_};
    // Retire the prior delayed load before saving exception-visible registers.
    auto saved=cpu;
    if (saved.load_register) saved.registers[saved.load_register]=saved.load_value;
    const auto status=(cpu.status&~0x3fU)|((cpu.status&15)<<2);
    store(ram,*frame,voluntary);
    for (unsigned reg=16;reg<=23;++reg) store(ram,*frame+reg*4,saved.registers[reg]);
    for (unsigned reg=28;reg<=31;++reg) store(ram,*frame+reg*4,saved.registers[reg]);
    store(ram,*frame+8,cpu.registers[4]); store(ram,*frame+12,cpu.registers[5]);
    store(ram,*frame+0x88,status); store(ram,*frame+0x8c,cpu.pc+4);
    store(ram,*frame+0x90,cpu.registers[6]);
    next=saved; next.status=status; next.epc=cpu.pc;
    next.cause=(cpu.cause&~0x8000007cU)|0x20;
    next.registers[4]=pointer; next.registers[29]=pointer-16;
    next.registers[31]=library_.module().entry;
    next.pc=context_callback_; next.delay_slot=false; next.load_register=0;
    next.interrupt_depth=1;
    context_pending_=true;
    return {CallState::transferred};
}
CallResult Intrman::interrupt_schedule(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) {
    if (!interrupted_ || !context_callback_ || !kernel_calls_.empty()) return {};
    const auto& saved=*interrupted_;
    const auto stack=saved.registers[29];
    if (stack<frame_size) return {CallState::address_fault,0,stack};
    const auto pointer=stack-frame_size;
    const auto frame=frame_offset(pointer,ram.size());
    if (!frame) return {CallState::address_fault,0,pointer};
    if (!ram_offset(context_callback_,4,ram.size())) return {CallState::address_fault,0,context_callback_};
    store(ram,*frame,full);
    for (unsigned reg=1;reg<32;++reg) store(ram,*frame+reg*4,saved.registers[reg]);
    store(ram,*frame+0x80,saved.hi); store(ram,*frame+0x84,saved.lo);
    store(ram,*frame+0x88,(saved.status&~0x3fU)|((saved.status&15)<<2));
    store(ram,*frame+0x8c,saved.delay_slot ? saved.pc-4 : saved.pc);
    store(ram,*frame+0x90,1);
    next=cpu; next.pc=context_callback_; next.registers[4]=pointer;
    next.registers[31]=library_.module().entry;
    next.load_register=0; next.delay_slot=false; next.status&=~1U;
    context_pending_=true; preemption_pending_=false;
    return {CallState::transferred};
}
}
