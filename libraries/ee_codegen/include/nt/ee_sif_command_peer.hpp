#pragma once
#include <nt/ee_sif_link.hpp>
#include <nt/ee_sif_outbound.hpp>
#include <array>
#include <nt/sif_rpc_reply.hpp>

namespace nt::ee {
// Original native bootstrap command service. Its address is a configured message
// port in the peer namespace, not a firmware address or executable module.
class NativeSifCommandPeer {
public:
    NativeSifCommandPeer(NativeSifLink& link,Memory& memory,CodeOverlap protection,std::uint32_t port,NativeSifReceive* receiver=nullptr)
        : link_(link),memory_(memory),protection_(protection),port_(port),receiver_(receiver) {}
    NativeSifCommandPeer(const NativeSifCommandPeer&)=delete;
    NativeSifCommandPeer& operator=(const NativeSifCommandPeer&)=delete;
    bool connect(NativeSifOutbound& outbound) {
        if (connected_ || !protection_ || !port_ || port_%16 || port_>0x200000-112) return false;
        if (!outbound.connect({this,accept})) return false;
        connected_=true;
        // Both transport acceptance and command parsing exist before publication.
        link_.publish_peer(port_,0x30000);
        return true;
    }
    std::uint32_t receive_address() const { return receive_address_; }
    std::uint32_t software_register(unsigned index) const { return index<registers_.size() ? registers_[index] : 0; }
    std::uint32_t last_option() const { return last_option_; }
    std::uint32_t last_command() const { return last_command_; }
private:
    static std::uint32_t word(std::span<const std::uint8_t> bytes,unsigned offset) {
        std::uint32_t value; std::memcpy(&value,bytes.data()+offset,4); return value;
    }
    static IoRead accept(void* context,std::span<const NativeSifPacket> packets) {
        return static_cast<NativeSifCommandPeer*>(context)->receive(packets);
    }
    IoRead reply(std::span<const std::uint8_t> bytes) {
        if (!receiver_ || !memory_.completions || !receive_address_) return {Stop::unsupported_system};
        if (!receiver_->accepting()) return {Stop::budget,0};
        const auto delivered=receiver_->deliver(memory_,protection_,*memory_.completions,receive_address_,bytes);
        return {delivered,delivered==Stop::budget ? 1U : 0U};
    }
    IoRead receive(std::span<const NativeSifPacket> packets) {
        // Payload-bearing RPC and multi-descriptor publication need a session
        // owner; reject them atomically rather than partially applying a batch.
        if (!connected_ || packets.size()!=1) return {Stop::unsupported_system};
        const auto& packet=packets.front();
        if (packet.destination!=port_ || packet.bytes.size()<16) return {Stop::invalid_state};
        const auto header=word(packet.bytes,0),length=header&255;
        if ((header>>8) || length<16 || length>112 || ((length+15)&~15U)!=packet.bytes.size()) return {Stop::invalid_state};
        const auto command=word(packet.bytes,8),option=word(packet.bytes,12);
        last_command_=command; last_option_=option;
        if (command==0x80000002 && option==1) {
            // Publishing register zero acknowledges the available RPC dispatcher,
            // not any particular RPC server or successful game module startup.
            std::array<std::uint8_t,32> response{};
            const std::array<std::uint32_t,6> words{24,0,0x80000001,0,0,1};
            std::memcpy(response.data(),words.data(),24);
            const auto result=reply(response);
            if (result.status==Stop::budget && result.value==1) rpc_ready_=true;
            return result;
        }
        if (command==0x80000009) {
            if (!rpc_ready_) return {Stop::unsupported_system};
            if (length<36 || option) return {Stop::invalid_state};
            // No native servers are registered yet. The public bind protocol
            // reports absence using null server/buffer tokens, not fake success.
            const auto response=nt::sif::bind_reply(word(packet.bytes,20),word(packet.bytes,28),0,0);
            return reply(response);
        }
        if (command==0x80000001) {
            if (length<24 || option || word(packet.bytes,16)>=registers_.size()) return {Stop::invalid_state};
            registers_[word(packet.bytes,16)]=word(packet.bytes,20);
            return {Stop::budget,1};
        }
        if (command!=0x80000000 && command!=0x80000002) return {Stop::unsupported_system};
        if (option) return {Stop::unsupported_system}; // RPC bootstrap requires its service owner.
        if (length<20) return {Stop::invalid_state};
        const auto raw=word(packet.bytes,16);
        const auto address=runtime::default_ram_address(raw);
        if (!address || address%16 || !memory_.data || std::uint64_t(address)+112>memory_.size || std::uint64_t(address)+112>0x2000000U) {
            memory_.fault_address=raw; return {Stop::memory_fault};
        }
        if (protection_(address,112)) { memory_.fault_address=raw; return {Stop::code_write}; }
        receive_address_=address;
        if (command==0x80000002) link_.acknowledge_main(0x20000);
        return {Stop::budget,1};
    }
    NativeSifLink& link_;
    Memory& memory_;
    CodeOverlap protection_;
    std::uint32_t port_,receive_address_=0,last_command_=0,last_option_=0;
    std::array<std::uint32_t,32> registers_{};
    NativeSifReceive* receiver_;
    bool connected_=false,rpc_ready_=false;
};
}
