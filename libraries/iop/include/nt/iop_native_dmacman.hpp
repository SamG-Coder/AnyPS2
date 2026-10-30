#pragma once
#include <nt/iop_dmacman.hpp>
#include <nt/iop_native_services.hpp>

namespace nt::iop {
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
