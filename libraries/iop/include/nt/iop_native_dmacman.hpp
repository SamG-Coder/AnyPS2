#pragma once
#include <nt/iop_dmacman_invoke.hpp>
#include <nt/iop_native_services.hpp>

namespace nt::iop {
// Priority words for compiled sceSetDMAPriority and sceEnableDMAChannel.
// Channel, control, and transfer registers stay unsupported. This is not the
// diagnostic device set and it does not start a DMA transfer.
class DmacPriorityState final : public Devices {
public:
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes) override {
        if (bytes!=4) return {};
        if (address==0x1f8010f0) return priority_;
        if (address==0x1f801570) return priority2_;
        return {};
    }
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value) override {
        if (bytes!=4) return false;
        if (address==0x1f8010f0) { priority_=value; return true; }
        if (address==0x1f801570) { priority2_=value; return true; }
        return false;
    }
private:
    std::uint32_t priority_=0, priority2_=0;
};
// Host register provider for compiled dmacman imports. Binding this service does
// not execute a DMA transfer. Slice and start ordinals stop as unavailable.
class NativeDmacmanServices final : public NativeServices {
public:
    explicit NativeDmacmanServices(Devices& devices) : devices_(devices) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t>) override {
        if (request.kind!=NativeServiceKind::dmacman) return {};
        const auto result=dmacman_invoke(devices_,request.selector,request.arguments);
        if (result.status==DmacmanStatus::unavailable) return {NativeServiceStatus::unavailable};
        if (result.status==DmacmanStatus::returned) return {NativeServiceStatus::returned,result.value};
        return {};
    }
private:
    Devices& devices_;
};
}
