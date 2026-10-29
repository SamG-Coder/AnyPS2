#pragma once
#include <nt/iop_native_interrupts.hpp>
#include <nt/iop_native_sif.hpp>
#include <cstring>

namespace nt::iop {
// Native link coordination. Peer publication is an explicit action by the EE
// owner; it cannot be inferred from an ISO, a module name or a successful build.
class NativeSifControl final : public NativeServices {
public:
    explicit NativeSifControl(NativeInterruptServices& interrupts,NativeSifTransfers* transfers=nullptr)
        : interrupts_(interrupts), transfers_(transfers) {}
    void publish_main(std::uint32_t address,std::uint32_t flags) {
        main_address_=address; main_flags_|=flags; connected_=true;
    }
    void acknowledge_sub(std::uint32_t flags) { sub_flags_&=~flags; }
    bool ready() const { return initialized_ && !stopped_; }
    std::uint32_t sub_address() const { return sub_address_; }
    std::uint32_t sub_flags() const { return sub_flags_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifman) return {};
        const auto returned=[](std::uint32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,value}; };
        if (request.selector==29) return returned(initialized_ ? 1 : 0);
        if (request.selector==2) {
            if (request.interrupt_context || interrupts_.invoke({23,{},NativeServiceKind::intrman},ram).value) return {};
            if (transfers_ && !transfers_->stop()) return {};
            receiving_=false; stopped_=true;
            interrupts_.invoke({7,{42,0,0,0},NativeServiceKind::intrman},ram);
            interrupts_.invoke({5,{42,0,0,0},NativeServiceKind::intrman},ram);
            // CheckInit records prior initialization, not current reception.
            return returned();
        }
        if (!connected_) return {};
        switch (request.selector) {
        case 5:
            if (stopped_ && !initialized_) return {};
            if (!initialized_) {
                if (!(main_flags_&0x10000)) return {};
                sub_address_=0; sub_flags_|=0x10000; receiving_=true; initialized_=true;
            }
            return returned();
        case 6:
            if (stopped_) return {}; // Reconnection requires a new native session.
            receiving_=true; return returned();
        case 21: return returned(main_flags_);
        case 22: main_flags_&=~request.arguments[0]; return returned(main_flags_);
        case 23: return returned(sub_flags_);
        case 24: sub_flags_|=request.arguments[0]; return returned(sub_flags_);
        case 25: return returned(main_address_);
        case 26: return returned(sub_address_);
        case 27: sub_address_=request.arguments[0]; return returned(sub_address_);
        default: return {};
        }
    }
    NativeServiceReply receive(std::span<const std::uint8_t> payload,std::uint32_t destination,std::span<std::uint8_t> ram) {
        if (!connected_ || !receiving_ || payload.empty() || payload.size()>0x100000 ||
            (payload.size()&15) || (destination&15)) return {};
        const auto offset=ram_offset(destination,1,ram.size());
        if (!offset || payload.size()>ram.size()-*offset || payload.size()>0x200000-*offset)
            return {NativeServiceStatus::address_fault,0,destination};
        for (auto at=*offset;at<*offset+payload.size();at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,destination};
        std::memmove(ram.data()+*offset,payload.data(),payload.size());
        receiving_=false;
        interrupts_.post(43); // Notify the compiled SIF1 consumer only after data is visible.
        return {NativeServiceStatus::returned};
    }
private:
    NativeInterruptServices& interrupts_;
    NativeSifTransfers* transfers_;
    std::uint32_t main_address_=0,sub_address_=0,main_flags_=0,sub_flags_=0;
    bool connected_=false,initialized_=false,receiving_=false,stopped_=false;
};
}
