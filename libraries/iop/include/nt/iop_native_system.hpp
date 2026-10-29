#pragma once
#include <nt/iop_native_boot.hpp>
#include <nt/iop_native_events.hpp>

namespace nt::iop {
// The bootstrap owns this event for its lifetime. It is an ordinary shared event,
// not a fabricated status value or an independent shadow of the event service.
class NativeSystemServices final : public NativeServices {
public:
    NativeSystemServices(NativeEventServices& events,NativeBootServices& boot,std::span<std::uint8_t> ram) {
        const auto mode=boot.invoke({12,{4,0,0,0},NativeServiceKind::loadcore},ram);
        if (mode.status!=NativeServiceStatus::returned) throw std::runtime_error("Invalid system-status boot context");
        std::uint32_t bits=0;
        if (mode.value) {
            const auto offset=ram_offset(mode.value,4,ram.size());
            if (!offset) throw std::runtime_error("Invalid system-status boot record");
            bits=1U<<(load_native_word(ram,*offset)&3);
        }
        const auto event=events.create(2,0,0);
        if (event<0) throw std::runtime_error("Cannot allocate native system-status event");
        event_=static_cast<std::uint32_t>(event);
        if (bits) events.invoke({6,{event_,bits,0,0},NativeServiceKind::thevent},ram);
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t>) override {
        if (request.kind!=NativeServiceKind::thbase || request.selector!=41) return {};
        return {NativeServiceStatus::returned,event_};
    }
private:
    std::uint32_t event_;
};
}
