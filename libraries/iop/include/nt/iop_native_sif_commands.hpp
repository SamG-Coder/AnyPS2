#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_sif_endpoint.hpp>
#include <cstring>

namespace nt::iop {
// Native command dispatch with an explicit peer-owned receive buffer. No SIF
// initialization, EE readiness or RPC service is inferred from registration.
class NativeSifCommands final : public NativeServices, public NativeCallbackSource {
public:
    explicit NativeSifCommands(NativeSifCommandEndpoint* peer=nullptr) : peer_(peer) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifcmd) return {};
        if (request.selector==12 || request.selector==13 || request.selector==28 || request.selector==29) return send(request,ram);
        const auto& a=request.arguments;
        if (request.selector==8 || request.selector==9) {
            if (a[1]>1024) return {};
            const auto stride=request.selector==8 ? 8U : 12U;
            if (a[1] && !range(a[0],a[1]*stride,ram)) return {NativeServiceStatus::address_fault,0,a[0]};
            tables_[request.selector-8]={a[0],a[1],stride};
            if (request.selector==9) system_table_configured_=true;
            return {NativeServiceStatus::returned};
        }
        if (request.selector!=10 && request.selector!=11) return {};
        const auto address=request.selector==10 ? a[1] : 0;
        if (owns_system_slot(a[0])) {
            if (address && !compiled(address)) return {NativeServiceStatus::unmapped_code,0,address};
            system_handlers_[a[0]&0x7fffffffU]=Handler{address,request.selector==10 ? a[2] : 0};
            return {NativeServiceStatus::returned};
        }
        const auto slot=handler_slot(a[0],ram);
        if (!slot) return {};
        if (address && !compiled(address)) return {NativeServiceStatus::unmapped_code,0,address};
        for (auto at=*slot;at<*slot+8;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,at};
        store_native_word(ram,*slot,address);
        store_native_word(ram,*slot+4,request.selector==10 ? a[2] : 0);
        return {NativeServiceStatus::returned};
    }
    // The native command module owns its default registrations. Explicit guest
    // tables retain the public ABI, including their caller-owned third word.
    void initialize_system_handlers() { system_handlers_initialized_=true; }
    bool uses_builtin_handler(std::uint32_t id) const {
        return (id&0x80000000U) && !system_table_configured_ &&
            !(owns_system_slot(id) && system_handlers_[id&0x7fffffffU]);
    }
    bool busy() const { return pending_.has_value() || active_; }
    // Caller owns buffer storage and must reserve it until busy() becomes false.
    // One accepted packet at a time prevents a later arrival overwriting a sliced
    // callback's arguments. Extra-data transfers require a separate peer contract.
    NativeServiceReply receive(std::span<const std::uint8_t> packet,std::uint32_t buffer,std::span<std::uint8_t> ram) {
        if (busy() || packet.size()<16 || packet.size()>112 || packet[0]!=packet.size() ||
            packet[1] || packet[2] || packet[3]) return {};
        const auto id=load_native_word(packet,8);
        Handler handler;
        if (owns_system_slot(id)) handler=system_handlers_[id&0x7fffffffU].value_or(Handler{});
        else {
            const auto slot=handler_slot(id,ram);
            if (!slot) return {};
            handler={load_native_word(ram,*slot),load_native_word(ram,*slot+4)};
        }
        if (!handler.address) return {NativeServiceStatus::returned}; // A removed command has no consumer.
        if (!compiled(handler.address)) return {NativeServiceStatus::unmapped_code,0,handler.address};
        const auto output=range(buffer,static_cast<std::uint32_t>(packet.size()),ram);
        if (!output) return {NativeServiceStatus::address_fault,0,buffer};
        for (auto at=*output;at<*output+packet.size();at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,at};
        // Do not overwrite either registered table with packet storage.
        for (const auto& table : tables_) if (table.count) {
            const auto start=range(table.address,table.count*table.stride,ram);
            if (start && *output<*start+table.count*table.stride && *start<*output+packet.size()) return {};
        }
        std::memmove(ram.data()+*output,packet.data(),packet.size());
        pending_=NativeCallback{handler.address,buffer,handler.argument};
        return {NativeServiceStatus::returned,1};
    }
    std::optional<NativeCallback> take() override {
        if (active_ || !pending_) return {};
        const auto callback=pending_; pending_.reset(); active_=true; return callback;
    }
    void finish(std::uint32_t) override {
        if (!active_) throw std::logic_error("Native command completion has no active callback");
        active_=false;
    }
private:
    bool owns_system_slot(std::uint32_t id) const {
        return system_handlers_initialized_ && !system_table_configured_ && (id&0x80000000U) &&
            (id&0x7fffffffU)<system_handlers_.size();
    }
    NativeServiceReply send(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        const bool with_callback=request.selector==28 || request.selector==29;
        if (!peer_ || (!(request.selector&1) && request.interrupt_context)) return {};
        if (a[2]<16 || a[2]>112) return {NativeServiceStatus::returned};
        const auto packet_offset=range(a[1],a[2],ram);
        if (!packet_offset) return {NativeServiceStatus::address_fault,0,a[1]};
        for (auto at=*packet_offset;at<*packet_offset+12;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,a[1]};
        const auto stack_bytes=with_callback ? 16U : 8U;
        if (request.stack_pointer>0xffffffffU-16-stack_bytes) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto stack=range(request.stack_pointer+16,stack_bytes,ram);
        if (!stack) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        std::optional<NativeCallback> callback;
        if (with_callback) {
            const auto address=load_native_word(ram,*stack+8);
            if (address) {
                if (!compiled(address)) return {NativeServiceStatus::unmapped_code,0,address};
                callback=NativeCallback{address,load_native_word(ram,*stack+12)};
            }
        }
        const auto destination=load_native_word(ram,*stack),raw_size=load_native_word(ram,*stack+4);
        const auto size=(raw_size&0x80000000U) ? 0U : raw_size;
        if (size>0x100000) return {};
        std::optional<std::uint32_t> source;
        if (size) {
            source=ram_offset(a[3],1,ram.size());
            if (!source || size>ram.size()-*source || size>0x200000-*source)
                return {NativeServiceStatus::address_fault,0,a[3]};
        }
        std::vector<std::uint8_t> packet(ram.begin()+*packet_offset,ram.begin()+*packet_offset+a[2]);
        store_native_word(packet,0,a[2]|(size<<8));
        store_native_word(packet,4,size ? destination : 0);
        store_native_word(packet,8,a[0]);
        std::vector<std::uint8_t> extra;
        if (size) {
            extra.assign(ram.begin()+*source,ram.begin()+*source+size);
            // The public ABI updates the packet header before reading payload.
            // Preserve that ordering when caller buffers overlap.
            for (unsigned byte=0;byte<12;++byte) {
                const auto at=*packet_offset+byte;
                if (at>=*source && at-*source<size) extra[at-*source]=packet[byte];
            }
        }
        const auto result=peer_->submit(packet,extra,callback);
        if (result.status==NativeServiceStatus::returned)
            std::copy_n(packet.begin(),12,ram.begin()+*packet_offset);
        return result;
    }
    static std::optional<std::uint32_t> range(std::uint32_t address,std::uint32_t size,std::span<std::uint8_t> ram) {
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {};
        return offset;
    }
    static bool compiled(std::uint32_t address) {
        const auto offset=ram_offset(address,4,0x200000);
        return offset && nt_iop_native_instruction(*offset);
    }
    std::optional<std::uint32_t> handler_slot(std::uint32_t id,std::span<std::uint8_t> ram) const {
        const auto& table=tables_[id>>31];
        const auto index=id&0x7fffffffU;
        if (index>=table.count) return {};
        const auto start=range(table.address,table.count*table.stride,ram);
        if (!start) return {};
        return *start+index*table.stride;
    }
    struct Table { std::uint32_t address=0,count=0,stride=0; };
    struct Handler { std::uint32_t address=0,argument=0; };
    NativeSifCommandEndpoint* peer_;
    std::array<Table,2> tables_{};
    // The public IOP command module has 32 default system slots. An empty optional
    // retains native protocol handling; an explicitly removed slot suppresses it.
    std::array<std::optional<Handler>,32> system_handlers_{};
    std::optional<NativeCallback> pending_;
    bool active_=false,system_table_configured_=false,system_handlers_initialized_=false;
};
}
