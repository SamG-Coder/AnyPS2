#pragma once
#include <array>
#include <nt/iop_devices.hpp>

namespace nt::iop {
enum class DmacmanStatus { unsupported, returned, unavailable };
struct DmacmanResult { DmacmanStatus status=DmacmanStatus::unsupported; std::uint32_t value=0; };

// Register reads, priority updates and channel enables. Ordinals 28 and 32 are
// unavailable and do not mutate devices or report a finished transfer.
inline DmacmanResult dmacman_invoke(Devices& devices,std::uint32_t ordinal,const std::array<std::uint32_t,4>& a) {
    const auto stopped=[](DmacmanStatus status,std::uint32_t value=0) { return DmacmanResult{status,value}; };
    // Channel execution stays an explicit stop. Do not clear CHCR or return success.
    if (ordinal==28 || ordinal==32) return stopped(DmacmanStatus::unavailable);
    std::optional<std::uint32_t> address;
    auto value=a[0];
    if (ordinal>=4 && ordinal<=11) {
        address=DmaController::channel_register(a[0],((ordinal-4)/2)*4);
        value=a[1];
    } else if (ordinal==14 || ordinal==15) address=0x1f8010f0;
    else if (ordinal==16 || ordinal==17) address=0x1f801570;
    else if (ordinal==26 || ordinal==27) address=0x1f801578;
    else if (ordinal>=33 && ordinal<=35) {
        if (a[0]>=13 || (ordinal==33 && a[1]>7)) return {};
        address=a[0]<7 ? 0x1f8010f0 : 0x1f801570;
        const auto previous=devices.read(*address,4);
        if (!previous) return {};
        const auto shift=(a[0]<7 ? a[0] : a[0]-7)*4;
        value=ordinal==33 ? (*previous&~(7U<<shift))|(a[1]<<shift) :
              ordinal==34 ? *previous|(8U<<shift) : *previous&~(8U<<shift);
        if (!devices.write(*address,4,value)) return {};
        return stopped(DmacmanStatus::returned);
    }
    if (!address) return {};
    if (ordinal&1) {
        const auto result=devices.read(*address,4);
        return result ? stopped(DmacmanStatus::returned,*result) : DmacmanResult{};
    }
    return devices.write(*address,4,value) ? stopped(DmacmanStatus::returned) : DmacmanResult{};
}
}
