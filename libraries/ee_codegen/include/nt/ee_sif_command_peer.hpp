#pragma once
#include <nt/ee_sif_link.hpp>
#include <nt/ee_sif_outbound.hpp>
#include <array>
#include <algorithm>
#include <string_view>
#include <optional>
#include <nt/sif_rpc_reply.hpp>
#include <nt/ee_rpc_service.hpp>

namespace nt::ee {
struct NativeSifResetRequest {
    std::uint32_t mode;
    std::string_view arguments;
};
// The owner prepares a fresh compiled session before accepting reset. A zero
// result retains the immutable request for retry; failure leaves the old peer
// intact. Accepted requests switch subsequent traffic to the owner's sink.
struct NativeSifResetOwner {
    void* context=nullptr;
    IoRead (*reset)(void*,const NativeSifResetRequest&)=nullptr;
    NativeSifSink commands;
};
// Original native bootstrap command service. Its address is a configured message
// port in the peer namespace, not a firmware address or executable module.
class NativeSifCommandPeer {
public:
    NativeSifCommandPeer(NativeSifLink& link,Memory& memory,CodeOverlap protection,std::uint32_t port,NativeSifReceive* receiver=nullptr)
        : link_(link),memory_(memory),protection_(protection),port_(port),receiver_(receiver) {}
    NativeSifCommandPeer(const NativeSifCommandPeer&)=delete;
    NativeSifCommandPeer& operator=(const NativeSifCommandPeer&)=delete;
    bool reset_owner(NativeSifResetOwner owner) {
        if (connected_ || reset_owner_.reset || !owner.reset || !owner.commands.accept) return false;
        reset_owner_=owner; return true;
    }
    bool connect(NativeSifOutbound& outbound) {
        if (connected_ || !protection_ || !port_ || port_%16 || port_>0x200000-112) return false;
        if (!outbound.connect({this,accept})) return false;
        connected_=true;
        // Both transport acceptance and command parsing exist before publication.
        link_.publish_peer(port_,0x30000);
        return true;
    }
    bool register_service(NativeRpcService& service) {
        if (connected_ || !service.server || !service.buffer || service.server>=0x200000 ||
            service.buffer%16 || service.buffer>0x200000-4096 || service.server==port_ || service.buffer==port_) return false;
        for (const auto* existing:services_)
            if (existing->id==service.id || existing->server==service.server || existing->buffer==service.buffer) return false;
        services_.push_back(&service); return true;
    }
    // Reuse native RPC services after an independently owned compiled handshake.
    // This entry does not publish link flags or initialize the command session.
    bool owns_rpc(std::span<const NativeSifPacket> packets) const {
        if (packets.empty()) return false;
        const auto& bytes=packets.back().bytes;
        if (bytes.size()<36) return false;
        const auto command=word(bytes,8);
        for (const auto* service:services_) {
            if (command==0x80000009 && word(bytes,32)==service->id) return true;
            if (command==0x8000000a && bytes.size()>=56 && word(bytes,52)==service->server) return true;
        }
        return false;
    }
    IoRead dispatch_rpc(std::span<const NativeSifPacket> packets,std::uint32_t receive_address,bool ready) {
        if (!ready) return {Stop::unsupported_system};
        if (!protection_ || !port_ || port_%16 || port_>0x200000-112) return {Stop::invalid_state};
        if (packets.empty() || packets.size()>2) return {Stop::invalid_state};
        const auto& packet=packets.back();
        if (packet.destination!=port_ || packet.bytes.size()<16) return {Stop::invalid_state};
        const auto header=word(packet.bytes,0),length=header&255;
        if (length<16 || length>112 || ((length+15)&~15U)!=packet.bytes.size()) return {Stop::invalid_state};
        const auto command=word(packet.bytes,8),option=word(packet.bytes,12);
        if (command==0x8000000a) {
            if (!ready || length<56 || option || word(packet.bytes,48)!=1) return {Stop::unsupported_system};
            NativeRpcService* service=nullptr;
            for (auto* item:services_) if (item->server==word(packet.bytes,52)) service=item;
            if (!service) return {Stop::unsupported_system};
            const auto size=word(packet.bytes,36);
            if (size>4096 || (header>>8)!=size || packets.size()!=(size ? 2U : 1U)) return {Stop::invalid_state};
            std::span<const std::uint8_t> args;
            if (size) {
                const auto& payload=packets.front();
                if (payload.destination!=service->buffer || word(packet.bytes,4)!=service->buffer ||
                    payload.bytes.size()!=((size+15U)&~15U)) return {Stop::invalid_state};
                args=std::span(payload.bytes).first(size);
            }
            if (!receiver_ || !memory_.completions || !receive_address) return {Stop::unsupported_system};
            if (!receiver_->accepting()) return {Stop::budget,0};
            const auto valid=receiver_->validate(memory_,protection_,receive_address,64);
            if (valid!=Stop::budget) return {valid};
            std::array<std::uint8_t,64> response{};
            const std::array<std::uint32_t,12> fields{64,0,0x80000008U,0,
                word(packet.bytes,16),word(packet.bytes,20),word(packet.bytes,24),word(packet.bytes,28),0x8000000aU,0,0,0};
            std::memcpy(response.data(),fields.data(),sizeof(fields));
            const auto called=service->call(memory_,protection_,word(packet.bytes,32),args,word(packet.bytes,40),word(packet.bytes,44));
            if (called!=Stop::budget) return {called};
            return reply_to(response,receive_address);
        }
        if (command!=0x80000009 || length<36 || option || (header>>8) || packets.size()!=1)
            return {Stop::invalid_state};
        for (const auto* service:services_) if (service->id==word(packet.bytes,32)) {
            const auto response=nt::sif::bind_reply(word(packet.bytes,20),word(packet.bytes,28),service->server,service->buffer);
            return reply_to(response,receive_address);
        }
        return {Stop::unsupported_system};
    }
    std::uint32_t receive_address() const { return receive_address_; }
    std::uint32_t software_register(unsigned index) const { return index<registers_.size() ? registers_[index] : 0; }
    std::uint32_t last_option() const { return last_option_; }
    std::optional<std::uint32_t> last_unavailable_server() const { return unavailable_server_; }
    std::uint32_t last_command() const { return last_command_; }
private:
    static std::uint32_t word(std::span<const std::uint8_t> bytes,unsigned offset) {
        std::uint32_t value; std::memcpy(&value,bytes.data()+offset,4); return value;
    }
    static IoRead accept(void* context,std::span<const NativeSifPacket> packets) {
        return static_cast<NativeSifCommandPeer*>(context)->receive(packets);
    }
    IoRead reply(std::span<const std::uint8_t> bytes) { return reply_to(bytes,receive_address_); }
    IoRead reply_to(std::span<const std::uint8_t> bytes,std::uint32_t address) {
        if (!receiver_ || !memory_.completions || !address) return {Stop::unsupported_system};
        if (!receiver_->accepting()) return {Stop::budget,0};
        const auto delivered=receiver_->deliver(memory_,protection_,*memory_.completions,address,bytes);
        return {delivered,delivered==Stop::budget ? 1U : 0U};
    }
    IoRead receive(std::span<const NativeSifPacket> packets) {
        if (handed_off_) return reset_owner_.commands.accept(reset_owner_.commands.context,packets);
        if (!connected_ || packets.empty() || packets.size()>2) return {Stop::unsupported_system};
        const auto& packet=packets.back();
        if (packet.destination!=port_ || packet.bytes.size()<16) return {Stop::invalid_state};
        const auto header=word(packet.bytes,0),length=header&255;
        if (length<16 || length>112 || ((length+15)&~15U)!=packet.bytes.size()) return {Stop::invalid_state};
        const auto command=word(packet.bytes,8),option=word(packet.bytes,12);
        last_command_=command; last_option_=option;
        if (command==0x8000000a) return dispatch_rpc(packets,receive_address_,rpc_ready_);
        if ((header>>8) || packets.size()!=1) return {Stop::invalid_state};
        if (command==0x80000003) {
            if (!reset_owner_.reset) return {Stop::unsupported_system};
            if (option || length<24) return {Stop::invalid_state};
            const auto count=word(packet.bytes,16);
            if (count>79 || count>length-24) return {Stop::invalid_state};
            const auto bytes=std::span(packet.bytes).subspan(24,count);
            if (std::find(bytes.begin(),bytes.end(),0)!=bytes.end()) return {Stop::invalid_state};
            const NativeSifResetRequest request{word(packet.bytes,20),
                {reinterpret_cast<const char*>(bytes.data()),bytes.size()}};
            const auto accepted=reset_owner_.reset(reset_owner_.context,request);
            if (accepted.status!=Stop::budget || !accepted.value) return accepted;
            if (accepted.value!=1) return {Stop::invalid_state};
            handed_off_=true;
            rpc_ready_=false; receive_address_=0; registers_.fill(0); unavailable_server_.reset();
            // New link addresses/flags are exclusively owned by the session.
            return accepted;
        }
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
            if (owns_rpc(packets)) return dispatch_rpc(packets,receive_address_,rpc_ready_);
            const auto response=nt::sif::bind_reply(word(packet.bytes,20),word(packet.bytes,28),0,0);
            const auto result=reply(response);
            if (result.status==Stop::budget && result.value==1) unavailable_server_=word(packet.bytes,32);
            return result;
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
    std::vector<NativeRpcService*> services_;
    bool connected_=false,rpc_ready_=false;
    std::optional<std::uint32_t> unavailable_server_;
    NativeSifResetOwner reset_owner_;
    bool handed_off_=false;
};
}
