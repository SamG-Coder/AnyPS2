#include <nt/iop_intrman.hpp>
#include <nt/iop_memory.hpp>

namespace nt::iop {
namespace {
void store_word(std::span<std::uint8_t> ram,std::uint32_t offset,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) ram[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
}
CallResult returned(std::int32_t value=0) { return {CallState::returned,static_cast<std::uint32_t>(value)}; }
}
Intrman::Intrman(BootstrapDevices& devices,std::uint32_t base)
    : controller_(devices.interrupts()), devices_(devices), library_("intrman",0x102,32,base) {}
CallResult Intrman::invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& a,std::span<std::uint8_t> ram,bool interrupt_context) {
    const auto ordinal=library_.ordinal(pc,ram.size());
    if (!ordinal) return {};
    const auto output=[&](std::uint32_t pointer) { return ram_offset(pointer,4,ram.size()); };
    switch (*ordinal) {
    case 4:
        if (interrupt_context) return returned(-100);
        if (a[0]>=handlers_.size()) return returned(-101);
        if (a[1]>2) return {};
        if (!a[2] || !ram_offset(a[2],4,ram.size())) return {CallState::address_fault,0,a[2]};
        if (handlers_[a[0]]) return returned(-104);
        handlers_[a[0]]=InterruptHandler{a[2],a[3],a[1]};
        return returned();
    case 5:
        if (interrupt_context) return returned(-100);
        if (a[0]>=handlers_.size()) return returned(-101);
        if (!handlers_[a[0]]) return returned(-105);
        handlers_[a[0]].reset();
        return returned();
    case 6: case 7: {
        const auto destination=output(a[1]);
        if (*ordinal==7 && a[1] && !destination) return {CallState::address_fault,0,a[1]};
        const auto irq=a[0]&0xff;
        if (a[0]&~0x3ffU) return {};
        if (irq>=32) {
            if (!((irq>=32 && irq<=38) || (irq>=40 && irq<=45))) return returned(-101);
            const bool second=irq>=40;
            const auto index=irq-(second ? 40U : 32U);
            const auto address=second ? 0x1f801574U : 0x1f8010f4U;
            const auto previous=*devices_.read(address,4)&0xffffff;
            const auto bit=1U<<(16+index);
            auto prior_irq=irq;
            if (!second && (previous&(1U<<index))) prior_irq|=0x100;
            const auto flags2=*devices_.read(0x1f801574,4);
            const auto tag_bit=1U<<(second ? index+7 : index);
            if (flags2&tag_bit) prior_irq|=0x200;
            if (*ordinal==6) {
                auto first=*devices_.read(0x1f8010f4,4)&0xffffff;
                auto next2=flags2&0xffffff;
                next2=(next2&~tag_bit)|((a[0]&0x200) ? tag_bit : 0);
                if (second) next2|=bit;
                else first=(first&~(1U<<index))|((a[0]&0x100) ? 1U<<index : 0)|bit;
                devices_.write(0x1f801574,4,next2);
                devices_.write(0x1f8010f4,4,first|0x800000);
                controller_.write(0x1f801074,4,*controller_.read(0x1f801074,4)|8);
                return returned();
            }
            const auto result=(previous&bit) ? 0 : -103;
            devices_.write(address,4,previous&~bit);
            if (a[1]) store_word(ram,*destination,result==0 ? prior_irq : static_cast<std::uint32_t>(-103));
            return returned(result);
        }
        const auto mask=*controller_.read(0x1f801074,4), bit=1U<<irq;
        controller_.write(0x1f801074,4,*ordinal==6 ? mask|bit : mask&~bit);
        if (*ordinal==6) return returned();
        const auto result=(mask&bit) ? 0 : -103;
        if (a[1]) store_word(ram,*destination,(mask&bit) ? irq : static_cast<std::uint32_t>(-103));
        return returned(result);
    }
    case 8: {
        const auto state=*controller_.read(0x1f801078,4);
        return returned(state ? 0 : -102);
    }
    case 9: controller_.write(0x1f801078,4,1); return returned();
    case 17: case 19: {
        const auto destination=output(a[0]);
        if (a[0] && !destination) return {CallState::address_fault,0,a[0]};
        const auto state=*controller_.read(0x1f801078,4);
        if (a[0]) store_word(ram,*destination,state);
        return returned(state ? 0 : -102);
    }
    case 18: case 20:
        if (a[0]>1) return {};
        controller_.write(0x1f801078,4,a[0]);
        return returned();
    case 23: return returned(interrupt_context ? 1 : 0);
    case 28: case 30:
        if (a[0] && !ram_offset(a[0],4,ram.size())) return {CallState::address_fault,0,a[0]};
        if (*ordinal==28) context_callback_=a[0];
        else preemption_callback_=a[0];
        return returned();
    case 29: context_callback_=0; return returned();
    case 31: preemption_callback_=0; return returned();
    default: return {};
    }
}
}
