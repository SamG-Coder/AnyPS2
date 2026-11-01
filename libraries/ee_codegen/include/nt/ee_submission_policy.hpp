#pragma once
#include <nt/ee_native.hpp>

namespace nt::ee {
// Compatibility policy for native packet submission. Host calls serialize work;
// console bus arbitration, stall control and MFIFO need dedicated translations.
class NativeSubmissionPolicy {
public:
    static bool maps(std::uint32_t address) {
        return address==0x1000e000 || address==0x1000e020 || address==0x1000f520 || address==0x1000f590;
    }
    bool enabled() const { return (control_&1) && !(enable_&0x10000); }
    std::uint32_t completion_selection() const { return priority_&0x3ffU; }
    IoRead read(std::uint32_t address) const {
        if (address==0x1000e000) return {Stop::budget,control_};
        if (address==0x1000e020) return {Stop::budget,priority_};
        if (address==0x1000f520 || address==0x1000f590) return {Stop::budget,enable_};
        return {Stop::unsupported_system};
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address==0x1000e000) {
            if (value&~1U) return Stop::unsupported_system;
            control_=value; return Stop::budget;
        }
        if (address==0x1000e020) {
            if (value&0x80000000U) return Stop::unsupported_system;
            priority_=value&0x03ff03ffU; return Stop::budget;
        }
        if (address==0x1000f590) { enable_=value; return Stop::budget; }
        return Stop::unsupported_system;
    }
private:
    std::uint32_t control_=0,priority_=0,enable_=0x1201;
};
}
