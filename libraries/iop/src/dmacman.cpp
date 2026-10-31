#include <nt/iop_dmacman.hpp>

namespace nt::iop {
CallResult Dmacman::invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& a,std::span<std::uint8_t> ram,bool) {
    const auto ordinal=library_.ordinal(pc,ram.size());
    if (!ordinal) return {};
    const auto result=dmacman_invoke(devices_,*ordinal,a);
    // The interpreter router has no unavailable state. An explicit DMA stop must
    // not fall through as a successful return.
    if (result.status==DmacmanStatus::returned) return {CallState::returned,result.value};
    return {};
}
}
