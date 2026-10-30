#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <atomic>

namespace nt::iop {
// Native loader ABI ownership. Callback registration does not load a module,
// authenticate executable content, or mark any module resident.
class NativeModloadServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::loadcore && (request.selector==4 || request.selector==5)) {
            // AOT instructions are immutable and native data accesses are coherent.
            // Preserve the publication ordering without a simulated cache bank.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return {NativeServiceStatus::returned};
        }
        if (request.kind!=NativeServiceKind::modload) return {};
        if (request.interrupt_context) return {NativeServiceStatus::unavailable};
        const auto& a=request.arguments;
        if (request.selector==12 || request.selector==13) {
            const auto count=request.selector==12 ? 3U : 1U;
            for (unsigned index=0;index<count;++index) {
                if (!a[index]) continue;
                const auto offset=ram_offset(a[index],4,ram.size());
                if (!offset || !nt_iop_native_instruction(*offset))
                    return {NativeServiceStatus::unmapped_code,0,a[index]};
            }
            if (request.selector==12) {
                card_=a[0]; disc_=a[1]; callbacks_=a[2];
            } else path_=a[0];
            return {NativeServiceStatus::returned};
        }
        if (request.selector==14) {
            std::array<std::uint32_t,2> outputs{};
            for (unsigned index=0;index<outputs.size();++index) {
                const auto at=ram_offset(a[index],4,ram.size());
                if (!at || ram.size()-*at<4 || 0x200000-*at<4)
                    return {NativeServiceStatus::address_fault,0,a[index]};
                if (nt_iop_native_code_word(*at)) return {NativeServiceStatus::code_write,0,a[index]};
                outputs[index]=*at;
            }
            store_native_word(ram,outputs[0],path_);
            store_native_word(ram,outputs[1],callbacks_);
            return {NativeServiceStatus::returned};
        }
        // Known lifecycle ABIs can be compiled, but cannot succeed until their
        // native catalog, initialization and completion owner is connected.
        return {NativeServiceStatus::unavailable};
    }
private:
    std::uint32_t card_=0,disc_=0,path_=0,callbacks_=0;
};
}
