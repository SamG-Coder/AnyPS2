#include <nt/iop_dmacman.hpp>
#include <nt/iop_dma.hpp>

namespace nt::iop {
CallResult Dmacman::invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& a,std::span<std::uint8_t> ram,bool) {
    const auto ordinal=library_.ordinal(pc,ram.size());
    if (!ordinal) return {};
    std::optional<std::uint32_t> address;
    auto value=a[0];
    if (*ordinal>=4 && *ordinal<=11) {
        address=DmaController::channel_register(a[0],((*ordinal-4)/2)*4);
        value=a[1];
    } else if (*ordinal==14 || *ordinal==15) address=0x1f8010f0;
    else if (*ordinal==16 || *ordinal==17) address=0x1f801570;
    else if (*ordinal==26 || *ordinal==27) address=0x1f801578;
    else if (*ordinal>=33 && *ordinal<=35) {
        if (a[0]>=13 || (*ordinal==33 && a[1]>7)) return {};
        address=a[0]<7 ? 0x1f8010f0 : 0x1f801570;
        const auto previous=devices_.read(*address,4);
        if (!previous) return {};
        const auto shift=(a[0]<7 ? a[0] : a[0]-7)*4;
        value=*ordinal==33 ? (*previous&~(7U<<shift))|(a[1]<<shift) :
              *ordinal==34 ? *previous|(8U<<shift) : *previous&~(8U<<shift);
        if (!devices_.write(*address,4,value)) return {};
        return {CallState::returned};
    }
    if (!address) return {};
    if (*ordinal&1) {
        const auto result=devices_.read(*address,4);
        return result ? CallResult{CallState::returned,*result} : CallResult{};
    }
    return devices_.write(*address,4,value) ? CallResult{CallState::returned} : CallResult{};
}
}
