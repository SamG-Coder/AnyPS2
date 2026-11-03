#pragma once
#include <nt/iop_native_sif_commands.hpp>
#include <nt/iop_native_sif_session.hpp>
#include <nt/iop_native_rpc.hpp>
#include <nt/iop_native_rpc_client.hpp>

namespace nt::iop {
// One serialized inbound mailbox, polled only at the native interrupt domain's
// delivery boundaries. The owner reserves packet_buffer for this source's life.
class NativeSifInbound final : public NativeCallbackSource {
public:
    NativeSifInbound(std::span<std::uint8_t> ram,std::uint32_t packet_buffer)
        : ram_(ram),packet_buffer_(packet_buffer) {}
    void connect(NativeSifCommands& commands,NativeSifCommandSession& session,NativeSifControl& link,NativeRpcServers* rpc=nullptr,NativeRpcClients* clients=nullptr) {
        if (commands_) throw std::logic_error("Native SIF inbound source already connected");
        commands_=&commands; session_=&session; link_=&link; rpc_=rpc; clients_=clients;
    }
    bool busy() const { return size_ || active_ || failed_.has_value() || (commands_ && commands_->busy()); }
    std::span<const std::uint8_t> pending_packet() const { return std::span<const std::uint8_t>(packet_).first(size_); }
    NativeServiceReply submit(std::span<const std::uint8_t> packet,std::span<const std::uint8_t> payload={}) {
        if (!commands_ || !link_->ready() || busy() || packet.size()<16 || packet.size()>packet_.size() ||
            packet[0]!=packet.size() || (load_native_word(packet,0)>>8)!=payload.size() || payload.size()>0x100000) return {};
        std::vector<std::uint8_t> owned(payload.begin(),payload.end());
        std::copy(packet.begin(),packet.end(),packet_.begin()); payload_=std::move(owned); size_=packet.size();
        return {NativeServiceStatus::returned,1};
    }
    std::optional<NativeCallback> take() override {
        if (!size_ || active_ || failed_) return {};
        const auto packet=std::span<const std::uint8_t>(packet_).first(size_);
        const auto command=load_native_word(packet,8);
        NativeServiceReply result;
        if (link_->ready()) {
            if (clients_ && clients_->owns_reply(packet)) result=clients_->receive(packet,payload_);
            else if (bind_reply_ || (command==0x80000009 && rpc_ && commands_->uses_builtin_handler(command))) {
                if (!bind_reply_) {
                    std::array<std::uint8_t,64> reply{};
                    result=rpc_->prepare_bind(packet,ram_,reply);
                    if (result.status==NativeServiceStatus::returned && result.value==0) bind_reply_=reply;
                }
                if (bind_reply_) {
                    result=rpc_->send_bind_reply(*bind_reply_);
                    if (result.status==NativeServiceStatus::returned) {
                        if (!result.value) return {}; // Keep the snapshot until the peer accepts it.
                        result.value=0;
                    }
                }
            }
            else if (command==0x8000000a && rpc_ && commands_->uses_builtin_handler(command))
                result=rpc_->receive_call(packet,payload_,ram_);
            else if (command>=0x80000000 && command<=0x80000002 && commands_->uses_builtin_handler(command))
                result=session_->receive_control(packet,ram_);
            else if (command==0x8000000c && commands_->uses_builtin_handler(command))
                // Unlike an explicitly removed application handler, this is an
                // unimplemented built-in request with a waiting RPC caller.
                // Do not drop it or fabricate a firmware/module-list snapshot.
                result={NativeServiceStatus::unavailable,0,packet.size()>=36 ? load_native_word(packet,32) : 0};
            else result=commands_->receive(packet,packet_buffer_,ram_);
        }
        if (result.status!=NativeServiceStatus::returned || result.value>1) {
            const auto reason=result.status==NativeServiceStatus::address_fault ? NativeStop::address_fault :
                result.status==NativeServiceStatus::code_write ? NativeStop::code_write :
                result.status==NativeServiceStatus::unmapped_code ? NativeStop::unmapped_code : NativeStop::service_request;
            failed_=NativeResult{reason,0,result.status==NativeServiceStatus::returned ? result.value : result.address,command};
            return {};
        }
        size_=0; payload_.clear(); bind_reply_.reset();
        const auto callback=commands_->take();
        active_=callback.has_value(); return callback;
    }
    void finish(std::uint32_t result) override {
        if (!active_) throw std::logic_error("Native inbound source has no active callback");
        commands_->finish(result); active_=false;
    }
    std::optional<NativeResult> failure() const override { return failed_; }
    // Explicit recovery/drop by the owner; never abandon a running callback.
    bool discard() {
        if (active_) return false;
        size_=0; payload_.clear(); failed_.reset(); bind_reply_.reset(); return true;
    }
private:
    std::span<std::uint8_t> ram_;
    std::uint32_t packet_buffer_;
    NativeSifCommands* commands_=nullptr;
    NativeSifCommandSession* session_=nullptr;
    NativeSifControl* link_=nullptr;
    NativeRpcServers* rpc_=nullptr;
    NativeRpcClients* clients_=nullptr;
    std::optional<std::array<std::uint8_t,64>> bind_reply_;
    std::array<std::uint8_t,112> packet_{};
    std::size_t size_=0;
    std::vector<std::uint8_t> payload_;
    std::optional<NativeResult> failed_;
    bool active_=false;
};
}
