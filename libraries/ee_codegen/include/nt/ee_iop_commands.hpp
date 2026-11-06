#pragma once
#include <nt/ee_sif_outbound.hpp>
#include <nt/iop_native_sif_inbound.hpp>

namespace nt::ee {
// Routes immutable EE command batches to an initialized compiled IOP session.
// Link publication and scheduling remain with the session owner; this adapter
// never manufactures initialization flags or runs a guest instruction decoder.
class NativeIopCommands {
public:
    // Without ram, only command batches are accepted.
    NativeIopCommands(iop::NativeSifControl& control,iop::NativeSifInbound& inbound,std::span<std::uint8_t> ram={},
                      bool (*compiled)(std::uint32_t)=nullptr)
        : control_(control),inbound_(inbound),ram_(ram),compiled_(compiled) {}
    NativeSifSink sink() { return {this,accept}; }
    // A batch not ending at the command buffer is plain DMA into IOP memory.
    bool plain_transfer(std::span<const NativeSifPacket> packets) const {
        return !packets.empty() && control_.sub_address() && packets.back().destination!=control_.sub_address();
    }
private:
    static IoRead accept(void* context,std::span<const NativeSifPacket> packets) {
        return static_cast<NativeIopCommands*>(context)->receive(packets);
    }
    IoRead receive(std::span<const NativeSifPacket> packets) {
        if (!control_.ready() || !control_.sub_address()) return {Stop::unsupported_system};
        if (plain_transfer(packets)) return write_memory(packets);
        if (packets.empty() || packets.size()>2) return {Stop::invalid_state};
        const auto& command=packets.back();
        if (command.destination!=control_.sub_address() || command.bytes.size()<16) return {Stop::invalid_state};
        const auto header=iop::load_native_word(command.bytes,0),size=header&255U,payload_size=header>>8;
        if (size<16 || size>112 || command.bytes.size()!=((size+15U)&~15U) ||
            payload_size>0x100000 || (payload_size && packets.size()!=2U)) return {Stop::invalid_state};
        std::span<const std::uint8_t> payload;
        if (payload_size) {
            const auto& data=packets.front();
            if (data.destination!=iop::load_native_word(command.bytes,4) ||
                data.bytes.size()!=((payload_size+15U)&~15U)) return {Stop::invalid_state};
            payload=std::span(data.bytes).first(payload_size);
        }
        if (inbound_.busy()) return {Stop::budget,0};
        // A command that declares no payload can still follow its own data
        // transfer in the same batch; that data is a plain memory write.
        if (packets.size()==2 && !payload_size) {
            const auto written=write_memory(packets.first(1));
            if (written.status!=Stop::budget) return written;
        }
        const auto result=inbound_.submit(std::span(command.bytes).first(size),payload);
        if (result.status!=iop::NativeServiceStatus::returned) return {Stop::unsupported_system};
        if (result.value!=1) return {Stop::invalid_state};
        return {Stop::budget,1};
    }
    IoRead write_memory(std::span<const NativeSifPacket> packets) {
        if (ram_.empty()) return {Stop::unsupported_system};
        // Validate the whole batch first: acceptance is atomic. Compiled code
        // cannot change, so a write over it stops rather than running stale.
        for (const auto& packet:packets) {
            if (std::uint64_t(packet.destination)+packet.bytes.size()>ram_.size()) return {Stop::invalid_state};
            if (compiled_) for (std::size_t offset=0;offset<packet.bytes.size();offset+=4)
                if (compiled_(packet.destination+static_cast<std::uint32_t>(offset))) return {Stop::unsupported_system};
        }
        // Earlier commands are observed first, as with one ordered channel.
        if (inbound_.busy()) return {Stop::budget,0};
        for (const auto& packet:packets) std::copy(packet.bytes.begin(),packet.bytes.end(),ram_.begin()+packet.destination);
        return {Stop::budget,1};
    }
    iop::NativeSifControl& control_;
    iop::NativeSifInbound& inbound_;
    std::span<std::uint8_t> ram_;
    bool (*compiled_)(std::uint32_t)=nullptr;
};
}
