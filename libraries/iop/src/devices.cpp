#include <nt/iop_devices.hpp>

namespace nt::iop {
std::optional<std::uint32_t> InterruptController::read(std::uint32_t address,unsigned bytes) {
    if (bytes!=4) return {};
    switch (address) {
    case 0x1f801070: return status_;
    case 0x1f801074: return mask_;
    case 0x1f801078: {
        const auto previous=control_;
        control_=0;
        return previous;
    }
    default: return {};
    }
}
bool InterruptController::write(std::uint32_t address,unsigned bytes,std::uint32_t value) {
    if (bytes!=4) return false;
    switch (address) {
    case 0x1f801070: status_&=value; return true;
    case 0x1f801074: mask_=value; return true;
    case 0x1f801078:
        if (value>1) return false; // Only the documented suspend/resume gate.
        control_=value; return true;
    default: return false;
    }
}
std::optional<std::uint32_t> BootstrapDevices::read(std::uint32_t address,unsigned bytes) {
    if (sbus_ && bytes==4 && address>=0x1d000000 && address<=0x1d000060 && !(address&15))
        return sbus_->read_sbus(address-0x1d000000);
    if (const auto value=interrupts_.read(address,bytes)) return value;
    if (const auto value=dma_.read(address,bytes)) return value;
    if (const auto value=timers_.read(address,bytes)) return value;
    if (const auto value=cdvd_.read(address,bytes)) return value;
    if (const auto value=resources_.read(address,bytes)) return value;
    if (address==0x1f801450 && bytes==4) return configuration_;
    if (address==0x1f801578 && bytes==4) return dma_control_;
    if (address==0xfffe0130 && bytes==4) return cache_control_;
    return {};
}
bool BootstrapDevices::write(std::uint32_t address,unsigned bytes,std::uint32_t value) {
    if (sbus_ && bytes==4 && address>=0x1d000000 && address<=0x1d000060 && !(address&15))
        return sbus_->write_sbus(address-0x1d000000,value);
    if (interrupts_.write(address,bytes,value)) return true;
    if (dma_.write(address,bytes,value)) {
        if (dma_.interrupt_pending()) interrupts_.raise(8);
        return true;
    }
    if (timers_.write(address,bytes,value)) return true;
    if (cdvd_.write(address,bytes,value)) {
        if (cdvd_.interrupt_pending()) interrupts_.raise(4);
        return true;
    }
    if (address==0xfffe0130 && bytes==4) {
        // Diagnostic coherent-memory backend: preserve configuration without
        // inventing cache hits, misses or stale instruction fetches.
        cache_control_=value;
        return true;
    }
    if (address==0x1f801578 && bytes==4) {
        dma_control_=value;
        return true;
    }
    if (address!=0x1f801450 || bytes!=4 || (value&8)) return false;
    // ICFG is retained configuration. PS1 compatibility mode is unsupported.
    configuration_=value;
    return true;
}
}
