#pragma once
#include <nt/iop_native_libraries.hpp>

namespace nt::iop {
// DEV9 owns an optional expansion adapter, not ordinary host sockets. There is
// currently no native raw-network/storage provider. Report failed initialization
// through the module ABI without manufacturing registers, a MAC or completions.
class NativeDev9Services final : public NativeServices {
public:
    explicit NativeDev9Services(NativeLibraryServices& libraries) : libraries_(libraries) {}
    std::uint64_t unavailable_initializations() const { return unavailable_initializations_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t>) override {
        if (request.kind!=NativeServiceKind::dev9) return {};
        if (request.selector!=0x100 || request.interrupt_context ||
            !libraries_.declaration_for_entry("dev9",1,request.call_site))
            return {NativeServiceStatus::unavailable};
        ++unavailable_initializations_;
        // MODULE_NO_RESIDENT_END: the loader releases this attempted module and
        // its argument/stack allocations. No exports or drivers are registered.
        return {NativeServiceStatus::returned,1};
    }
private:
    NativeLibraryServices& libraries_;
    std::uint64_t unavailable_initializations_=0;
};
}
