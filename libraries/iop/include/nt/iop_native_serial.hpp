#pragma once
#include <nt/iop_native_services.hpp>
#include <array>

namespace nt::iop {
// Session-owned controller/card configuration. FIFO and transfer responses are
// unsupported until a native transport supplies their real completion state.
class NativeSerialServices final : public NativeServices {
public:
    void reset() { commands_={}; ports_={}; control_=0; interrupts_=0; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t>) override {
        if (request.kind!=NativeServiceKind::serial) return {};
        const auto address=request.arguments[0], value=request.arguments[1];
        if ((address&3U) || request.selector>1) return {NativeServiceStatus::unavailable};
        const bool write=request.selector==1;
        if (address>=0x1f808200 && address<0x1f808260) {
            const auto offset=(address-0x1f808200)/4;
            auto& setting=offset<16 ? commands_[offset] : ports_[offset-16];
            if (write) setting=value;
            return {NativeServiceStatus::returned,setting};
        }
        if (address==0x1f808268) {
            if (write) {
                if (value&1U) return {NativeServiceStatus::unavailable};
                if (value&0xcU) interrupts_=0;
                control_=value;
            }
            return {NativeServiceStatus::returned,control_};
        }
        if (address==0x1f808280) {
            if (write) interrupts_&=~value;
            return {NativeServiceStatus::returned,interrupts_};
        }
        return {NativeServiceStatus::unavailable};
    }
private:
    std::array<std::uint32_t,16> commands_{};
    std::array<std::uint32_t,8> ports_{};
    std::uint32_t control_=0,interrupts_=0;
};
}
