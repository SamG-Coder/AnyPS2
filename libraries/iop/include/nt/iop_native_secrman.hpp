#pragma once
#include <nt/iop_native_services.hpp>

namespace nt::iop {
// Ordinals 4 and 5 store memory-card handler pointers. A zero argument clears
// the stored pointer. The handlers are not called. Ordinal 6 reports that no
// card authenticated. Download and card-data ordinals stay unsupported.
class NativeSecrmanServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t>) override {
        if (request.kind!=NativeServiceKind::secrman) return {};
        if (request.selector==4) { command_=request.arguments[0]; return {NativeServiceStatus::returned,0}; }
        if (request.selector==5) { device_=request.arguments[0]; return {NativeServiceStatus::returned,0}; }
        if (request.selector==6) return {NativeServiceStatus::returned,0};
        return {};
    }
    std::uint32_t command_handler() const { return command_; }
    std::uint32_t device_handler() const { return device_; }
private:
    std::uint32_t command_=0, device_=0;
};
}
