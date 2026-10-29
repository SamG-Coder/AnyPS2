#pragma once
#include <nt/runtime.hpp>

namespace nt::hardware {
// Only idle-unit control is modeled. Every path that starts VU execution stops.
class VuControl {
public:
    std::uint32_t read(unsigned reg) const {
        if (reg == 28) return fbrst_;
        if (reg == 29) return 0; // VPU_STAT: both units idle.
        throw runtime::MemoryFault("VU: control-register read is not implemented");
    }
    void write(unsigned reg, std::uint32_t value) {
        if (reg == 29) return; // VPU_STAT is read-only.
        if (reg != 28) throw runtime::MemoryFault("VU: control-register write/execution is not implemented");
        fbrst_ = value & 0x0c0c;
        if (value & 2) fbrst_ &= ~0xffU;
        if (value & 0x200) fbrst_ &= ~0xff00U;
        // Force-break strobes have no outstanding execution to interrupt.
    }
private:
    std::uint32_t fbrst_ = 0;
};
}
