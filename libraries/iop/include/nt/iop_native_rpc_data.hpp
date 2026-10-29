#pragma once
#include <nt/iop_native_sif_endpoint.hpp>

namespace nt::iop {
// A connected converted peer permits direct reads. Both synchronous and NOWAIT
// calls complete at this serialized native boundary, without a guest RPC packet.
class NativeRpcDataServices final : public NativeServices {
public:
    explicit NativeRpcDataServices(NativeSifCommandEndpoint* peer=nullptr) : peer_(peer) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifcmd || request.selector!=23 || request.interrupt_context ||
            !peer_ || !peer_->accepting() || next_id_>0x7fffffffU) return {};
        const auto& a=request.arguments;
        if (a[3]>0x100000) return {};
        if (request.stack_pointer>0xffffffffU-16) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
        if (!stack || ram.size()-*stack<4 || 0x200000-*stack<4) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        if (load_native_word(ram,*stack)>1) return {};
        const auto descriptor=ram_offset(a[0],4,ram.size());
        if (!descriptor || ram.size()-*descriptor<28 || 0x200000-*descriptor<28)
            return {NativeServiceStatus::address_fault,0,a[0]};
        const auto checked=writable(ram,a[0],12,4);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        std::optional<std::uint32_t> destination;
        if (a[3]) {
            const auto output=writable(ram,a[2],a[3],1);
            if (output.status!=NativeServiceStatus::returned) return output;
            destination=ram_offset(a[2],1,ram.size());
            if (*destination<*descriptor+28 && *descriptor<*destination+a[3]) return {};
        }
        std::vector<std::uint8_t> payload;
        const auto read=peer_->snapshot(a[1],a[3],payload);
        if (read.status!=NativeServiceStatus::returned) return read;
        if (destination) std::copy(payload.begin(),payload.end(),ram.begin()+*destination);
        store_native_word(ram,*descriptor,0); // Completed; CheckStatRpc observes no outstanding packet.
        store_native_word(ram,*descriptor+4,next_id_++);
        store_native_word(ram,*descriptor+8,0xffffffff); // No semaphore needed for immediate completion.
        return {NativeServiceStatus::returned};
    }
private:
    static NativeServiceReply writable(std::span<std::uint8_t> ram,std::uint32_t address,std::uint32_t size,unsigned alignment) {
        const auto offset=ram_offset(address,alignment,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,address};
        for (auto at=*offset&~3U;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    NativeSifCommandEndpoint* peer_;
    std::uint32_t next_id_=1;
};
}
