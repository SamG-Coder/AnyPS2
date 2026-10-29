#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <algorithm>
#include <array>
#include <string>

namespace nt::iop {
// Own registrations and compiled lifecycle continuations, never host function
// pointers obtained from guest data. File services retain registrations while open.
class NativeDriverServices final : public NativeServices {
public:
    struct Device { std::string name; std::uint32_t address=0,deinit=0,global_pointer=0,operations=0,type=0; unsigned users=0; };
    std::optional<std::size_t> find(const std::string& name) const {
        for (std::size_t slot=0;slot<devices_.size();++slot)
            if (devices_[slot] && devices_[slot]->name==name) return slot;
        return {};
    }
    const Device& device(std::size_t slot) const { return devices_.at(slot).value(); }
    void retain(std::size_t slot) { ++devices_.at(slot)->users; }
    void release(std::size_t slot) { --devices_.at(slot)->users; }
    std::size_t size() const {
        return std::count_if(devices_.begin(),devices_.end(),[](const auto& device) { return device.has_value(); });
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) return finish(request);
        if (request.kind!=NativeServiceKind::ioman) return {};
        if (request.selector!=20 && request.selector!=21) {
            // Do not let another provider manufacture ENODEV or operate on an
            // unrelated native mount after a compiled driver has registered.
            return pending_ ? NativeServiceReply{NativeServiceStatus::unavailable} : NativeServiceReply{};
        }
        if (request.interrupt_context || !request.context) return {NativeServiceStatus::unavailable};
        if (pending_) return returned(-1);
        Device device;
        std::size_t slot=0;
        std::uint32_t target=0;
        const bool adding=request.selector==20;
        if (adding) {
            while (slot<devices_.size() && devices_[slot]) ++slot;
            if (slot==devices_.size()) return returned(-1);
            const auto descriptor=extent(request.arguments[0],20,ram);
            if (!descriptor) return fault(request.arguments[0]);
            device.address=request.arguments[0]; device.type=load_native_word(ram,*descriptor+4);
            const auto name=read_name(load_native_word(ram,*descriptor),ram,device.name);
            if (name.status!=NativeServiceStatus::returned) return name;
            const auto operations=load_native_word(ram,*descriptor+16);
            const auto table=extent(operations,8,ram);
            if (!table) return fault(operations);
            target=load_native_word(ram,*table); device.deinit=load_native_word(ram,*table+4);
            device.global_pointer=request.global_pointer; device.operations=operations;
            if (!compiled(device.deinit)) return {NativeServiceStatus::unmapped_code,0,device.deinit};
        } else {
            std::string name;
            const auto checked=read_name(request.arguments[0],ram,name);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            while (slot<devices_.size() && (!devices_[slot] || devices_[slot]->name!=name)) ++slot;
            if (slot==devices_.size()) return returned(-1);
            device=*devices_[slot]; target=device.deinit;
            if (device.users) return {NativeServiceStatus::unavailable};
            const auto descriptor=extent(device.address,20,ram);
            if (!descriptor) return fault(device.address);
            std::string current_name;
            const auto checked_name=read_name(load_native_word(ram,*descriptor),ram,current_name);
            if (checked_name.status!=NativeServiceStatus::returned) return checked_name;
            const auto operations=load_native_word(ram,*descriptor+16);
            const auto table=extent(operations,8,ram);
            if (!table) return fault(operations);
            if (current_name!=device.name || load_native_word(ram,*table+4)!=device.deinit)
                return {NativeServiceStatus::unavailable};

        }
        if (!compiled(target)) return {NativeServiceStatus::unmapped_code,0,target};
        Cpu caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc;
        caller.registers[0]=0;
        pending_=Pending{request.context,caller,std::move(device),slot,adding};
        NativeServiceReply reply{NativeServiceStatus::called,target};
        reply.call=NativeServiceCall{{pending_->device.address,0,0,0},pending_->device.global_pointer,this};
        return reply;
    }
private:
    struct Pending { const Cpu* owner; Cpu caller; Device device; std::size_t slot; bool adding; };
    static bool compiled(std::uint32_t address) {
        const auto code=ram_offset(address,4,0x200000);
        return address && code && nt_iop_native_instruction(*code);
    }
    static std::optional<std::uint32_t> extent(std::uint32_t address,std::uint32_t bytes,std::span<const std::uint8_t> ram) {
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset || bytes>ram.size()-*offset || bytes>0x200000-*offset) return {};
        return offset;
    }
    static NativeServiceReply returned(std::int32_t value) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    static NativeServiceReply fault(std::uint32_t address) { return {NativeServiceStatus::address_fault,0,address}; }
    static NativeServiceReply read_name(std::uint32_t address,std::span<const std::uint8_t> ram,std::string& result) {
        const auto offset=ram_offset(address,1,ram.size());
        if (!offset) return fault(address);
        const auto count=std::min<std::size_t>({256,ram.size()-*offset,0x200000-*offset});
        const auto bytes=ram.subspan(*offset,count);
        const auto end=std::find(bytes.begin(),bytes.end(),0);
        if (end==bytes.end()) return count==256 ? NativeServiceReply{NativeServiceStatus::unavailable} : fault(address);
        if (end==bytes.begin()) return {NativeServiceStatus::unavailable};
        result.assign(bytes.begin(),end); return returned(0);
    }
    NativeServiceReply finish(const NativeServiceRequest& request) {
        if (!request.context || request.context->native_call_owner!=this || !pending_ || pending_->owner!=request.context) return {};
        const bool failed=pending_->adding && (request.arguments[0]&0x80000000U);
        if (pending_->adding) {
            if (!failed) devices_[pending_->slot]=std::move(pending_->device);
        } else devices_[pending_->slot].reset(); // Legacy DelDrv ignores deinit's result.
        auto caller=pending_->caller; caller.registers[2]=failed ? 0xffffffffU : 0;
        pending_.reset();
        NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
    }
    std::array<std::optional<Device>,16> devices_{};
    std::optional<Pending> pending_;
};
}
