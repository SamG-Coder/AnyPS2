#pragma once
#include <nt/ee_native.hpp>

namespace nt::ee {
// Compatibility policy for native packet submission. Host calls serialize work;
// console bus arbitration and stall control need dedicated translations. MFIFO
// routes scratchpad output into a RAM ring that the vector channel drains.
class NativeSubmissionPolicy {
public:
    void enable_native_queue() { control_=1; }
    static bool maps(std::uint32_t address) {
        return address==0x1000e000 || address==0x1000e020 || unused_transfer_configuration(address) || inactive_channel(address) ||
            address==0x1000f520 || address==0x1000f590;
    }
    bool enabled() const { return (control_&1) && !(enable_&0x10000); }
    std::uint32_t completion_selection() const { return priority_&0x3ffU; }
    bool mfifo_to_vector() const { return (control_&0xcU)==8U; }
    std::uint32_t mfifo_address(std::uint32_t address) const { return ring_base_|(address&ring_mask_); }
    IoRead read(std::uint32_t address) const {
        if (address==0x1000e000) return {Stop::budget,control_};
        if (address==0x1000e020) return {Stop::budget,priority_};
        if (address==0x1000e040) return {Stop::budget,ring_mask_};
        if (address==0x1000e050) return {Stop::budget,ring_base_};
        if (unused_transfer_configuration(address) || inactive_channel(address)) return {Stop::budget,0};
        if (address==0x1000f520 || address==0x1000f590) return {Stop::budget,enable_};
        return {Stop::unsupported_system};
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address==0x1000e040) { ring_mask_=value&0x1ffffff0U; return Stop::budget; }
        if (address==0x1000e050) { ring_base_=value&0x1ffffff0U; return Stop::budget; }
        // Direct native submissions have no interleave store. Accept initialization
        // to the disabled configuration, not activation.
        if (unused_transfer_configuration(address) || inactive_channel(address))
            return value ? Stop::unsupported_system : Stop::budget;
        if (address==0x1000e000) {
            // Cycle stealing shares a console bus that native transfers do not use.
            // Stall control changes transfers and needs a translation, and only
            // the vector channel has an MFIFO consumer.
            if ((value&0xf0U) || (value&0xcU)==4U || (value&0xcU)==12U) return Stop::unsupported_system;
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
    // These transfer endpoints have no native consumer yet. Their disabled,
    // empty configuration is valid; any configuration or activation requiring
    // work remains unsupported. Never publish completion for initialization.
    static bool inactive_channel(std::uint32_t address) {
        const auto base=address&~0xffU,field=address&0xffU;
        if (base!=0x1000c400 &&
            base!=0x1000c800) return false;
        return (field<=0x50 && field%16==0) || field==0x80;
    }
    static bool unused_transfer_configuration(std::uint32_t address) {
        return address==0x1000e030 || address==0x1000e040 || address==0x1000e050;
    }
    std::uint32_t control_=0,priority_=0,enable_=0x1201,ring_base_=0,ring_mask_=0;
};
}
