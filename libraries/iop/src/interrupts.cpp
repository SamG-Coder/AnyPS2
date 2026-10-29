#include <nt/iop_intrman.hpp>
#include <nt/iop_memory.hpp>
#include <bit>

namespace nt::iop {
namespace {
unsigned dma_irq(unsigned channel) { return channel<7 ? channel+32 : channel+33; }
std::uint32_t dma_control(unsigned channel) { return channel<7 ? 0x1f8010f4U : 0x1f801574U; }
unsigned dma_index(unsigned channel) { return channel<7 ? channel : channel-7; }
}
void Intrman::acknowledge_dma(unsigned channel) {
    const auto address=dma_control(channel);
    const auto control=*devices_.read(address,4)&0xffffff;
    devices_.write(address,4,control|(1U<<(24+dma_index(channel))));
}
CallResult Intrman::continue_dma(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) {
    if (devices_.forced_dma_interrupt()) return {CallState::pending};
    const auto pending=devices_.pending_dma_channels();
    if (!pending) { active_dma_.reset(); return {CallState::returned}; }
    const auto channel=static_cast<unsigned>(std::countr_zero(pending));
    const auto& handler=handlers_[dma_irq(channel)];
    if (!handler) return {CallState::pending};
    if (!ram_offset(handler->entry,4,ram.size())) return {CallState::address_fault,0,handler->entry};
    next=cpu; next.pc=handler->entry; next.registers[4]=handler->argument;
    next.registers[29]=library_.module().base+allocation_size()-32;
    next.registers[31]=library_.module().entry+12;
    next.status&=~1U; next.load_register=0; next.delay_slot=false;
    active_dma_=channel;
    acknowledge_dma(channel);
    return {CallState::transferred};
}
CallResult Intrman::interrupt(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) {
    if (!owns(library_.module().entry,ram.size())) return {};
    if (interrupted_ || context_pending_ || cpu.interrupt_depth) return {CallState::pending};
    const auto mask=*controller_.read(0x1f801074,4);
    const auto pending=*controller_.read(0x1f801070,4)&mask;
    if (!pending) return {CallState::pending};
    const auto irq=static_cast<unsigned>(std::countr_zero(pending));
    std::optional<unsigned> channel;
    if (irq==3) {
        const auto channels=devices_.pending_dma_channels();
        if (!channels || devices_.forced_dma_interrupt()) return {CallState::pending};
        channel=static_cast<unsigned>(std::countr_zero(channels));
    }
    const auto handler_irq=channel ? dma_irq(*channel) : irq;
    if (!handlers_[handler_irq]) return {CallState::pending};
    const auto handler=*handlers_[handler_irq];
    if (!ram_offset(handler.entry,4,ram.size())) return {CallState::address_fault,0,handler.entry};
    const auto stack=library_.module().base+allocation_size()-32;
    if (!ram_offset(stack,4,ram.size())) return {CallState::address_fault,0,stack};
    interrupted_=cpu;
    if (cpu.load_register) interrupted_->registers[cpu.load_register]=cpu.load_value;
    interrupted_->load_register=0;
    active_irq_=irq;
    active_dma_=channel;
    next=*interrupted_;
    next.epc=cpu.delay_slot ? cpu.pc-4 : cpu.pc;
    next.cause=(cpu.cause&~0x8000007fU)|(cpu.delay_slot ? 0x80000000U : 0);
    next.status=(cpu.status&~0x3fU)|((cpu.status&15)<<2);
    next.pc=handler.entry; next.delay_slot=false; next.interrupt_depth=1;
    next.registers[4]=handler.argument; next.registers[29]=stack;
    next.registers[31]=library_.module().entry+12;
    controller_.write(0x1f801074,4,mask&~(1U<<irq));
    if (channel) acknowledge_dma(*channel);
    controller_.write(0x1f801070,4,~(1U<<irq));
    return {CallState::transferred};
}
CallResult Intrman::restore_interrupted(const Cpu& cpu,Cpu& next) {
    next=*interrupted_;
    next.executed=cpu.executed;
    controller_.write(0x1f801078,4,1);
    interrupted_.reset(); preemption_pending_=false;
    return {CallState::transferred};
}
CallResult Intrman::interrupt_return(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram,unsigned ordinal) {
    if (!interrupted_ || context_pending_) return {};
    if (ordinal==1) {
        if (preemption_pending_) return {};
        if (preemption_callback_ && !ram_offset(preemption_callback_,4,ram.size()))
            return {CallState::address_fault,0,preemption_callback_};
        if (active_dma_) {
            if (!cpu.registers[2]) {
                const auto address=dma_control(*active_dma_);
                const auto control=*devices_.read(address,4)&0xffffff;
                devices_.write(address,4,control&~(1U<<(16+dma_index(*active_dma_))));
            }
            const auto chained=continue_dma(cpu,next,ram);
            if (chained.state!=CallState::returned) return chained;
            // All channel causes have drained. Drop the aggregate latch before
            // reenabling the cascade, leaving other primary sources untouched.
            controller_.write(0x1f801070,4,~8U);
        }
        if (active_irq_==3 || cpu.registers[2]) {
            const auto mask=*controller_.read(0x1f801074,4);
            controller_.write(0x1f801074,4,mask|(1U<<active_irq_));
        }
        if (!preemption_callback_) return restore_interrupted(cpu,next);
        next=cpu; next.pc=preemption_callback_; next.registers[4]=0;
        next.registers[31]=library_.module().entry+24;
        next.load_register=0; next.delay_slot=false; next.status&=~1U;
        preemption_pending_=true;
        return {CallState::transferred};
    }
    if (ordinal!=2 || !preemption_pending_) return {};
    if (cpu.registers[2]) return interrupt_schedule(cpu,next,ram);
    return restore_interrupted(cpu,next);
}
}
