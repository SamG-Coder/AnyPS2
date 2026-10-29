#pragma once
#include <nt/runtime.hpp>

namespace nt::hardware {
// Serialized VU0 macro operations; microprogram execution remains unsupported.
class VuControl {
public:
    runtime::Register vector(unsigned reg) const;
    void set_vector(unsigned reg, runtime::Register value);
    void add_subtract(unsigned destination, unsigned left, unsigned right, unsigned mask, bool subtract, unsigned broadcast);
    std::uint32_t read(unsigned reg) const {
        if (reg < 16) return reg ? integers_[reg] : 0;
        if (reg == 16) return status_;
        if (reg == 17) return mac_;
        if (reg == 28) return fbrst_;
        if (reg == 29) return 0; // VPU_STAT: both units idle.
        throw runtime::MemoryFault("VU: control-register read is not implemented");
    }
    void write(unsigned reg, std::uint32_t value) {
        if (reg < 16) { if (reg) integers_[reg] = static_cast<std::uint16_t>(value); return; }
        if (reg == 17) return; // MAC flags are read-only.
        if (reg == 29) return; // VPU_STAT is read-only.
        if (reg != 28) throw runtime::MemoryFault("VU: control-register write/execution is not implemented");
        fbrst_ = value & 0x0c0c;
        if (value & 2) fbrst_ &= ~0xffU;
        if (value & 0x200) fbrst_ &= ~0xff00U;
        // Force-break strobes have no outstanding execution to interrupt.
    }
private:
    std::uint32_t fbrst_ = 0;
    std::uint32_t mac_ = 0, status_ = 0;
    std::array<runtime::Register,32> vectors_{};
    std::array<std::uint16_t,16> integers_{};
};
}
