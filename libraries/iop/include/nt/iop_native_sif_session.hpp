#pragma once
#include <nt/iop_native_sif_control.hpp>
#include <nt/iop_native_sif_endpoint.hpp>
#include <nt/iop_native_system.hpp>

namespace nt::iop {
// Native command bootstrap uses the same status event as converted IOP clients.
// Incoming control packets must be supplied by an explicit converted EE owner.
class NativeSifCommandSession final : public NativeServices {
public:
    NativeSifCommandSession(NativeEventServices& events,NativeSystemServices& system,NativeSifControl& control,
                            NativeSifCommandEndpoint* peer=nullptr)
        : events_(events),control_(control),peer_(peer) {
        status_event_=system.invoke({41,{},NativeServiceKind::thbase},{}).value;
    }
    bool rpc_ready() const { return rpc_published_ && rpc_acknowledged_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifcmd) return {};
        const auto& a=request.arguments;
        if (request.selector==6 || request.selector==7) {
            if (a[0]>=registers_.size()) return {};
            if (request.selector==7) registers_[a[0]]=a[1];
            return {NativeServiceStatus::returned,request.selector==6 ? registers_[a[0]] : 0};
        }
        if (request.selector==14) return initialize_rpc(request,ram);
        if (request.selector!=4 || request.interrupt_context || !peer_ || !control_.ready()) return {};
        const auto waited=events_.invoke({10,{status_event_,0x100,0,0},NativeServiceKind::thevent},ram);
        if (waited.status==NativeServiceStatus::yielded || (waited.status==NativeServiceStatus::returned && waited.value==0))
            control_.invoke({24,{0x20000,0,0,0},NativeServiceKind::sifman},ram);
        return waited;
    }
    NativeServiceReply receive_control(std::span<const std::uint8_t> packet,std::span<std::uint8_t> ram) {
        if (!peer_ || !control_.ready() || packet.size()<16 || packet.size()>112 ||
            load_native_word(packet,0)!=packet.size()) return {};
        const auto command=load_native_word(packet,8);
        if (command==0x80000001) {
            if (packet.size()<24) return {};
            const auto index=load_native_word(packet,16);
            if (index>=registers_.size()) return {};
            registers_[index]=load_native_word(packet,20);
            return {NativeServiceStatus::returned};
        }
        if (command==0x80000002 && load_native_word(packet,12)) {
            const auto result=signal(0x800,ram);
            if (result.status==NativeServiceStatus::returned && !result.value) rpc_acknowledged_=true;
            return result;
        }
        if (packet.size()<20 || (command!=0x80000000 && command!=0x80000002)) return {};
        const auto address=load_native_word(packet,16);
        return peer_->publish_buffer(address,[&]() {
            if (command==0x80000000) return NativeServiceReply{NativeServiceStatus::returned};
            const auto result=signal(0x100,ram);
            if (result.status==NativeServiceStatus::returned && result.value==0)
                control_.invoke({22,{0x20000,0,0,0},NativeServiceKind::sifman},ram);
            return result;
        });
    }
private:
    NativeServiceReply initialize_rpc(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (request.interrupt_context || !peer_ || !control_.ready()) return {};
        // Command initialization can suspend, but RPC initialization must resume
        // here afterwards rather than return to its caller prematurely.
        const auto command=invoke({4,{},NativeServiceKind::sifcmd},ram);
        if (command.status==NativeServiceStatus::yielded) return {NativeServiceStatus::retry_yielded};
        if (command.status!=NativeServiceStatus::returned || command.value) return command;
        if (!rpc_published_) {
            // Validate the shared event before publishing anything to the peer.
            const auto ready=events_.invoke({11,{status_event_,0x800,0,0},NativeServiceKind::thevent},ram);
            if (ready.status!=NativeServiceStatus::returned || (ready.value && ready.value!=static_cast<std::uint32_t>(-421))) return ready;
            std::array<std::uint8_t,24> packet{};
            store_native_word(packet,0,24); store_native_word(packet,8,0x80000001);
            store_native_word(packet,16,0); store_native_word(packet,20,1);
            const auto sent=peer_->submit(packet,{});
            if (sent.status!=NativeServiceStatus::returned) return sent;
            if (!sent.value) return {NativeServiceStatus::waiting};
            rpc_published_=true;
        }
        const auto waited=events_.invoke({10,{status_event_,0x800,0,0},NativeServiceKind::thevent},ram);
        // A standalone caller may explicitly retry after its peer acknowledges.
        if (waited.status==NativeServiceStatus::unsupported) return {NativeServiceStatus::waiting};
        return waited;
    }
    NativeServiceReply signal(std::uint32_t bits,std::span<std::uint8_t> ram) {
        return events_.invoke({7,{status_event_,bits,0,0},NativeServiceKind::thevent,0,true},ram);
    }
    NativeEventServices& events_;
    NativeSifControl& control_;
    NativeSifCommandEndpoint* peer_;
    std::uint32_t status_event_;
    bool rpc_published_=false;
    bool rpc_acknowledged_=false;
    std::array<std::uint32_t,32> registers_{};
};
}
