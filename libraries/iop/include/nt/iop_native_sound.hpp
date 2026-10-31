#pragma once
#include <nt/audio.hpp>
#include <nt/iop_native_libraries.hpp>

namespace nt::iop {
// The converter lowers validated LIBSD public entries to this native owner.
// Unimplemented sound operations stop before changing state or reporting success.
class NativeSoundServices final : public NativeServices {
public:
    NativeSoundServices(NativeLibraryServices& libraries,audio::Output& output) : libraries_(libraries),output_(output) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::libsd) return {};
        if (request.interrupt_context) return {NativeServiceStatus::unavailable};
        if (request.selector==0) {
            if (registered_) return {NativeServiceStatus::unavailable};
            const auto declaration=libraries_.declaration_for_entry("libsd",1,request.call_site);
            if (!declaration || !output_.open()) return {NativeServiceStatus::unavailable};
            const auto registered=libraries_.invoke({6,{*declaration},NativeServiceKind::loadcore},ram);
            if (registered.status!=NativeServiceStatus::returned || registered.value) {
                output_.close(); return {NativeServiceStatus::unavailable};
            }
            registered_=true;
            return {NativeServiceStatus::returned};
        }
        if (!registered_) return {NativeServiceStatus::unavailable};
        if (request.selector==2) { output_.close(); return {NativeServiceStatus::returned}; }
        if (request.selector==4) {
            if (request.arguments[0]>1 || !output_.initialize(request.arguments[0]!=0))
                return {NativeServiceStatus::unavailable};
            return {NativeServiceStatus::returned};
        }
        return {NativeServiceStatus::unavailable};
    }
private:
    NativeLibraryServices& libraries_;
    audio::Output& output_;
    bool registered_=false;
};
}
