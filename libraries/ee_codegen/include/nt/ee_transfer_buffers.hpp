#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/ee_sif_outbound.hpp>
#include <nt/binary.hpp>
#include <map>

namespace nt::ee {
// Native IPC upload allocations, not an IOP RAM image. Tokens retain the public
// client ABI; only explicitly allocated ranges can receive immutable transfers.
class NativeTransferBuffers final : public NativeRpcService {
public:
    NativeTransferBuffers() : NativeRpcService(0x80000003,0xe000,0xf000) {}
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        if (method!=1 && method!=2) return Stop::unsupported_system;
        if (args.size()!=4 || result_size!=4) return Stop::invalid_state;
        const auto valid=rpc_output(memory,protection,destination,result_size);
        if (valid!=Stop::budget) return valid;
        std::uint32_t result=0;
        const auto value=le32(args,0);
        if (method==1) {
            if (value && value<=end-begin) {
                const auto size=(value+15)&~15U;
                auto address=begin;
                for (const auto& [base,data]:buffers_) {
                    if (address+size<=base) break;
                    address=base+static_cast<std::uint32_t>(data.size());
                }
                if (address+size<=end) {
                    buffers_.emplace(address,std::vector<std::uint8_t>(size)); result=address;
                }
            }
        } else if (!buffers_.erase(value)) result=0xffffffffU;
        std::memcpy(memory.data+runtime::default_ram_address(destination),&result,4);
        return Stop::budget;
    }
    IoRead upload(std::span<const NativeSifPacket> packets) {
        if (packets.empty() || packets.size()>32) return {Stop::invalid_state};
        for (const auto& packet:packets)
            if (packet.attributes || packet.bytes.empty() || view(packet.destination,packet.bytes.size()).empty())
                return {Stop::invalid_state};
        for (const auto& packet:packets) {
            auto found=std::prev(buffers_.upper_bound(packet.destination));
            std::copy(packet.bytes.begin(),packet.bytes.end(),found->second.begin()+packet.destination-found->first);
        }
        return {Stop::budget,1};
    }
    std::span<const std::uint8_t> view(std::uint32_t address,std::size_t size) const {
        auto found=buffers_.upper_bound(address);
        if (found==buffers_.begin()) return {};
        --found;
        const auto offset=address-found->first;
        if (offset>found->second.size() || size>found->second.size()-offset) return {};
        return std::span(found->second).subspan(offset,size);
    }
private:
    static constexpr std::uint32_t begin=0x20000,end=0x200000;
    std::map<std::uint32_t,std::vector<std::uint8_t>> buffers_;
};
}
