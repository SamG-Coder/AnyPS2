#include <nt/iop_timers.hpp>

namespace nt::iop {
namespace {
struct Register { unsigned index,offset; };
std::optional<Register> decode(std::uint32_t address,unsigned bytes) {
    if ((bytes!=2 && bytes!=4) || address%bytes) return {};
    unsigned index=0;
    if (address>=0x1f801100 && address<0x1f801130) index=(address-0x1f801100)/16;
    else if (address>=0x1f801480 && address<0x1f8014b0) index=3+(address-0x1f801480)/16;
    else return {};
    const auto offset=address&15;
    if (offset!=0 && offset!=4 && offset!=8 && !(index>=3 && bytes==2 && offset==2)) return {};
    return Register{index,offset};
}
std::uint32_t mask(unsigned index) { return index<3 ? 0xffffU : 0xffffffffU; }
bool supported(unsigned index,std::uint32_t mode) {
    if ((index==0 || index==1 || index==3) && (mode&0x101)) return false;
    return true;
}
}
void Timers::rearm(Counter& counter) {
    counter.mode=(counter.mode&~0x1800U)|0x400;
    counter.irq_mode=counter.mode&0xc0;
}
bool Timers::event(Counter& counter,bool overflow) {
    const auto flag=overflow ? 0x1000U : 0x800U;
    const auto enable=overflow ? 0x20U : 0x10U;
    bool raised=false;
    if (counter.mode&enable) {
        const bool repeat=(counter.irq_mode&0x40)!=0;
        const bool toggle=(counter.irq_mode&0x80)!=0;
        const bool armed=(counter.mode&0x400)!=0;
        raised=armed && (repeat || !(counter.mode&flag));
        if (repeat || (armed && toggle)) {
            if (toggle) counter.mode^=0x400;
            else counter.mode&=~0x400U;
        }
    }
    counter.mode|=flag;
    return raised;
}
std::optional<std::uint32_t> Timers::read(std::uint32_t address,unsigned bytes) {
    const auto reg=decode(address,bytes);
    if (!reg) return {};
    auto& counter=counters_[reg->index];
    std::uint32_t value=0;
    switch (reg->offset) {
    case 0: value=counter.count; break;
    case 2: value=counter.count>>16; break;
    case 4: value=counter.mode; rearm(counter); break;
    case 8: value=counter.target; break;
    }
    return bytes==2 ? value&0xffff : value;
}
bool Timers::write(std::uint32_t address,unsigned bytes,std::uint32_t value) {
    const auto reg=decode(address,bytes);
    if (!reg || reg->offset==2) return false;
    if (bytes==2) value&=0xffff;
    auto& counter=counters_[reg->index];
    switch (reg->offset) {
    case 0:
        counter.count=value&mask(reg->index);
        counter.target_pending=counter.count<=counter.target;
        return true;
    case 4:
        if (!supported(reg->index,value)) return false;
        {
            const bool preserve=(counter.mode&0x1800) && (counter.mode&0x30);
            counter.mode=(value&0x63ff)|(counter.mode&0x1c00);
            if (!preserve) rearm(counter);
        }
        counter.count=0; counter.phase=0;
        counter.target_pending=true;
        return true;
    case 8:
        counter.target=value&mask(reg->index);
        counter.target_pending=counter.target>counter.count;
        return true;
    default: return false;
    }
}
std::uint32_t Timers::tick() {
    constexpr unsigned divisors[]{1,8,16,256};
    constexpr unsigned causes[]{4,5,6,14,15,16};
    std::uint32_t interrupts=0;
    for (unsigned index=0;index<counters_.size();++index) {
        auto& counter=counters_[index];
        if ((counter.mode&1) && !(counter.mode&2)) continue;
        const auto divisor=index==2 ? ((counter.mode&0x200) ? 8U : 1U) :
                           index>=4 ? divisors[(counter.mode>>13)&3] : 1U;
        if (++counter.phase<divisor) continue;
        counter.phase=0;
        auto count=std::uint64_t(counter.count)+1;
        bool raised=false;
        if (counter.target_pending && count>=counter.target) {
            raised=event(counter,false);
            if (counter.mode&8) count-=counter.target;
            else counter.target_pending=false;
        }
        if (count>mask(index)) {
            raised=event(counter,true) || raised;
            counter.target_pending=true;
        }
        counter.count=static_cast<std::uint32_t>(count)&mask(index);
        if (raised) interrupts|=1U<<causes[index];
    }
    return interrupts;
}
}
