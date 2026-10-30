#pragma once
#include <nt/ee_sif_outbound.hpp>
#include <nt/iop_native_sif_inbound.hpp>

namespace nt::ee {
// Routes immutable EE command batches to an initialized compiled IOP session.
// Link publication and scheduling remain with the session owner; this adapter
// never manufactures initialization flags or runs a guest instruction decoder.
class NativeIopCommands {
public:
    NativeIopCommands(iop::NativeSifControl& control,iop::NativeSifInbound& inbound)
        : control_(control),inbound_(inbound) {}
    NativeSifSink sink() { return {this,accept}; }
private:
    static IoRead accept(void* context,std::span<const NativeSifPacket> packets) {
        return static_cast<NativeIopCommands*>(context)->receive(packets);
    }
    IoRead receive(std::span<const NativeSifPacket> packets) {
        if (!control_.ready() || !control_.sub_address()) return {Stop::unsupported_system};
        if (packets.empty() || packets.size()>2) return {Stop::invalid_state};
        const auto& command=packets.back();
        if (command.destination!=control_.sub_address() || command.bytes.size()<16) return {Stop::invalid_state};
        const auto header=iop::load_native_word(command.bytes,0),size=header&255U,payload_size=header>>8;
        if (size<16 || size>112 || command.bytes.size()!=((size+15U)&~15U) ||
            payload_size>0x100000 || packets.size()!=(payload_size ? 2U : 1U)) return {Stop::invalid_state};
        std::span<const std::uint8_t> payload;
        if (payload_size) {
            const auto& data=packets.front();
            if (data.destination!=iop::load_native_word(command.bytes,4) ||
                data.bytes.size()!=((payload_size+15U)&~15U)) return {Stop::invalid_state};
            payload=std::span(data.bytes).first(payload_size);
        }
        if (inbound_.busy()) return {Stop::budget,0};
        const auto result=inbound_.submit(std::span(command.bytes).first(size),payload);
        if (result.status!=iop::NativeServiceStatus::returned) return {Stop::unsupported_system};
        if (result.value!=1) return {Stop::invalid_state};
        return {Stop::budget,1};
    }
    iop::NativeSifControl& control_;
    iop::NativeSifInbound& inbound_;
};
}
