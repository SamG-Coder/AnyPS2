#pragma once
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_waits.hpp>
#include <nt/iop_native_sif_endpoint.hpp>
#include <deque>
#include <map>

namespace nt::iop {
// Outbound RPC ownership. A send only queues a request; a matching peer reply
// publishes results and resumes a suspended compiled continuation.
class NativeRpcClients final : public NativeServices,public NativeWaitQueue,public NativeCallbackSource {
public:
    NativeRpcClients(NativeHeapServices& heap,NativeThreadWaits& waits,NativeSifCommandEndpoint* peer,
                     std::function<bool()> ready,std::span<std::uint8_t> ram)
        : heap_(heap),waits_(waits),peer_(peer),ready_(std::move(ready)),ram_(ram) {}
    bool cancel_wait(std::uint32_t client,std::uint32_t thread) override {
        const auto found=pending_.find(client);
        if (found==pending_.end() || found->second.waiter!=thread) return false;
        found->second.waiter.reset(); return true;
    }
    bool owns_reply(std::span<const std::uint8_t> packet) const {
        if (packet.size()<32 || load_native_word(packet,8)!=0x80000008) return false;
        const auto client=ram_offset(load_native_word(packet,28),4,ram_.size());
        return client && pending_.contains(*client);
    }
    NativeServiceReply receive(std::span<const std::uint8_t> packet,std::span<const std::uint8_t> payload) {
        if (!owns_reply(packet) || packet.size()<48 || packet[0]!=packet.size() ||
            (load_native_word(packet,0)>>8)!=payload.size()) return unavailable();
        const auto client=*ram_offset(load_native_word(packet,28),4,ram_.size());
        auto& request=pending_.at(client);
        // Bind replies echo the request packet. Call completions use a peer's
        // reusable reply record: its packet/id fields are not correlation data.
        // The client owns exactly one pending call, whose live record must still
        // match before any output, callback or waiting continuation is released.
        const bool bind=request.command==0x80000009;
        if (request.complete || (bind && load_native_word(packet,20)!=request.packet) || load_native_word(packet,32)!=request.command ||
            load_native_word(ram_,client)!=request.packet || load_native_word(ram_,client+4)!=request.id) return unavailable();
        if (payload.size()!=request.receive_size || (!payload.empty() &&
            load_native_word(packet,4)!=request.receive_address)) return unavailable();
        auto checked=writable(client,40);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        checked=writable(request.packet,64);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (!payload.empty()) {
            checked=writable(request.receive_address,request.receive_size);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        if (request.callback) callbacks_.push_back(client);
        if (!payload.empty()) std::copy(payload.begin(),payload.end(),ram_.begin()+*ram_offset(request.receive_address,1,ram_.size()));
        if (request.command==0x80000009) {
            store_native_word(ram_,client+36,load_native_word(packet,36));
            store_native_word(ram_,client+20,load_native_word(packet,40));
        }
        request.complete=true;
        if (!request.callback) finish_client(client);
        return returned();
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifcmd ||
            (request.selector!=15 && request.selector!=16 && request.selector!=18)) return {};
        if (ram.data()!=ram_.data() || ram.size()!=ram_.size() || request.interrupt_context) return unavailable();
        const auto& a=request.arguments;
        const auto client=ram_offset(a[0],4,ram.size());
        if (!client || 40>ram.size()-*client || 40>0x200000-*client) return {NativeServiceStatus::address_fault,0,a[0]};
        if (request.selector==18) return returned(pending_.contains(*client));
        if (!peer_ || !peer_->accepting() || !ready_ || !ready_()) return unavailable();
        if (pending_.contains(*client) || pending_.size()>=32 || next_id_==0xffffffff) return returned(0xffffffff);
        const auto mode=a[2];
        if (mode>1) return unavailable();
        Pending next; next.command=request.selector==15 ? 0x80000009 : 0x8000000a; next.id=next_id_;
        next.waiter=mode ? std::nullopt : waits_.current_waiter();
        if (!mode && !next.waiter) return unavailable();
        auto checked=writable(a[0],40);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        std::vector<std::uint8_t> payload;
        std::uint32_t destination=0,server=0;
        if (request.selector==16) {
            if (request.stack_pointer>0xffffffffU-16) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
            if (!stack || 20>ram.size()-*stack || 20>0x200000-*stack) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto size=load_native_word(ram,*stack);
            next.receive_address=load_native_word(ram,*stack+4); next.receive_size=load_native_word(ram,*stack+8);
            next.callback=load_native_word(ram,*stack+12); next.argument=load_native_word(ram,*stack+16); next.gp=request.global_pointer;
            if (size>0x100000 || next.receive_size>0x100000) return unavailable();
            // Notification-free NOWAIT uses a different completion record;
            // keep it explicit until that protocol is implemented.
            if (mode && !next.callback) return unavailable();
            if (next.callback) {
                const auto target=ram_offset(next.callback,4,ram.size());
                if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,next.callback};
            }
            if (next.receive_size) {
                checked=writable(next.receive_address,next.receive_size);
                if (checked.status!=NativeServiceStatus::returned) return checked;
                const auto offset=*ram_offset(next.receive_address,1,ram.size());
                if (offset<*client+40 && *client<offset+next.receive_size) return unavailable();
            }
            if (size) {
                const auto source=ram_offset(a[3],1,ram.size());
                if (!source || size>ram.size()-*source || size>0x200000-*source) return {NativeServiceStatus::address_fault,0,a[3]};
                payload.assign(ram.begin()+*source,ram.begin()+*source+size);
            }
            destination=load_native_word(ram,*client+20); server=load_native_word(ram,*client+36);
            if (!server) return unavailable();
        }
        const auto allocated=heap_.invoke({4,{0,64},NativeServiceKind::sysmem},ram);
        if (allocated.status!=NativeServiceStatus::returned) return allocated;
        if (!allocated.value) return returned(0xffffffff);
        next.packet=allocated.value;
        checked=writable(next.packet,64);
        if (checked.status!=NativeServiceStatus::returned) { release(next.packet); return checked; }
        std::array<std::uint8_t,64> packet{};
        store_native_word(packet,0,64|(static_cast<std::uint32_t>(payload.size())<<8)); store_native_word(packet,4,destination);
        store_native_word(packet,8,next.command); store_native_word(packet,16,2); store_native_word(packet,20,next.packet);
        store_native_word(packet,24,next.id); store_native_word(packet,28,a[0]); store_native_word(packet,32,a[1]);
        if (request.selector==16) {
            store_native_word(packet,36,static_cast<std::uint32_t>(payload.size())); store_native_word(packet,40,next.receive_address);
            store_native_word(packet,44,next.receive_size); store_native_word(packet,48,1); store_native_word(packet,52,server);
        }
        try { pending_.emplace(*client,next); }
        catch (...) { release(next.packet); throw; }
        const auto sent=peer_->submit(packet,payload);
        if (sent.status!=NativeServiceStatus::returned || !sent.value) {
            pending_.erase(*client); release(next.packet);
            return sent.status==NativeServiceStatus::returned ? NativeServiceReply{NativeServiceStatus::waiting} : sent;
        }
        std::copy(packet.begin(),packet.end(),ram.begin()+next.packet);
        store_native_word(ram,*client,next.packet); store_native_word(ram,*client+4,next.id);
        store_native_word(ram,*client+8,0xffffffff); store_native_word(ram,*client+12,mode);
        if (request.selector==15) { store_native_word(ram,*client+16,0); store_native_word(ram,*client+36,0); }
        else { store_native_word(ram,*client+28,next.callback); store_native_word(ram,*client+32,next.argument); }
        ++next_id_;
        if (!mode) { waits_.suspend_wait(*next.waiter,*client,NativeWaitKind::rpc,*this); return {NativeServiceStatus::yielded}; }
        return returned();
    }
    std::optional<NativeCallback> take() override {
        if (active_ || callbacks_.empty()) return {};
        active_=callbacks_.front(); callbacks_.pop_front();
        const auto& request=pending_.at(*active_);
        return NativeCallback{request.callback,request.argument,{},request.gp};
    }
    void finish(std::uint32_t) override {
        if (!active_) throw std::logic_error("RPC client callback has no owner");
        finish_client(*active_); active_.reset();
    }
private:
    struct Pending {
        std::uint32_t packet=0,id=0,command=0,receive_address=0,receive_size=0,callback=0,argument=0,gp=0;
        std::optional<std::uint32_t> waiter;
        bool complete=false;
    };
    static NativeServiceReply returned(std::uint32_t value=0) { return {NativeServiceStatus::returned,value}; }
    static NativeServiceReply unavailable() { return {NativeServiceStatus::unavailable}; }
    NativeServiceReply writable(std::uint32_t pointer,std::uint32_t size) const {
        const auto offset=ram_offset(pointer,1,ram_.size());
        if (!offset || size>ram_.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        for (auto at=*offset&~3U;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,pointer};
        return returned();
    }
    void release(std::uint32_t packet) { heap_.invoke({5,{packet},NativeServiceKind::sysmem},ram_); }
    void finish_client(std::uint32_t client) {
        const auto request=pending_.at(client);
        store_native_word(ram_,request.packet+16,0); store_native_word(ram_,request.packet+24,0);
        store_native_word(ram_,client,0);
        release(request.packet); pending_.erase(client);
        if (request.waiter) waits_.resume_wait(*request.waiter,0);
    }
    NativeHeapServices& heap_;
    NativeThreadWaits& waits_;
    NativeSifCommandEndpoint* peer_;
    std::function<bool()> ready_;
    std::span<std::uint8_t> ram_;
    std::map<std::uint32_t,Pending> pending_;
    std::deque<std::uint32_t> callbacks_;
    std::optional<std::uint32_t> active_;
    std::uint32_t next_id_=1;
};
}
