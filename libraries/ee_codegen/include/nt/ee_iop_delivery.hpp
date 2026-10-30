#pragma once
#include <nt/ee_sif_receive.hpp>
#include <nt/iop_native_sif_endpoint.hpp>
#include <array>

namespace nt::ee {
// Direct compiled-IOP command delivery into the existing EE completion path.
// Both owners serialize access; neither side fetches instructions or DMA tags.
class NativeIopDelivery {
public:
    NativeIopDelivery(Memory& memory,CodeOverlap protection,NativeSifReceive& receiver)
        : memory_(memory),protection_(protection),receiver_(receiver) {}
    iop::NativeServiceReply operator()(std::uint32_t address,std::span<const std::uint8_t> command,
                                      std::span<const std::uint8_t> payload) const {
        using iop::NativeServiceStatus;
        if (!protection_ || !memory_.completions) return {};
        if (command.size()<16 || command.size()>112 || command[0]!=command.size() ||
            (iop::load_native_word(command,0)>>8)!=payload.size() || payload.size()>0x100000) return {};
        if (!receiver_.accepting()) return {NativeServiceStatus::returned};
        const auto padded=(command.size()+15)&~std::size_t(15);
        const auto checked=receiver_.validate(memory_,protection_,address,padded);
        if (checked!=Stop::budget) return failure(checked,address);
        const auto raw=iop::load_native_word(command,4),offset=runtime::default_ram_address(raw);
        if (!payload.empty()) {
            if (!memory_.data || std::uint64_t(offset)+payload.size()>memory_.size ||
                std::uint64_t(offset)+payload.size()>0x2000000)
                return {NativeServiceStatus::address_fault,0,raw};
            if (protection_(offset,static_cast<std::uint32_t>(payload.size())))
                return {NativeServiceStatus::code_write,0,raw};
        }
        // Snapshot aliases and allocate before making either region visible.
        std::array<std::uint8_t,112> packet{};
        std::copy(command.begin(),command.end(),packet.begin());
        const std::vector<std::uint8_t> data(payload.begin(),payload.end());
        if (!data.empty()) std::copy(data.begin(),data.end(),memory_.data+offset);
        // Validation above covers every fallible condition in deliver. Packet
        // publication follows payload writes, then raises the real EE completion.
        const auto delivered=receiver_.deliver(memory_,protection_,*memory_.completions,address,
                                               std::span(packet).first(padded));
        if (delivered!=Stop::budget) throw std::logic_error("Validated native IOP delivery changed during commit");
        return {NativeServiceStatus::returned,1};
    }
private:
    static iop::NativeServiceReply failure(Stop stop,std::uint32_t address) {
        using iop::NativeServiceStatus;
        if (stop==Stop::memory_fault) return {NativeServiceStatus::address_fault,0,address};
        if (stop==Stop::code_write) return {NativeServiceStatus::code_write,0,address};
        return {};
    }
    Memory& memory_;
    CodeOverlap protection_;
    NativeSifReceive& receiver_;
};
}
