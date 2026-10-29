#pragma once
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_code.hpp>
#include <vector>

namespace nt::iop {
// Startup ordering is explicit; all handlers are already compiled functions.
class NativeBootCallbacks final : public NativeServices {
public:
    explicit NativeBootCallbacks(NativeHeapServices& heap) : heap_(heap) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) {
            if (!pending_ || request.context!=pending_->owner || !request.context || request.context->native_call_owner!=this) return {};
            if (pending_->status) {
                const auto checked=writable(pending_->status,ram);
                if (checked.status!=NativeServiceStatus::returned) return checked;
            }
            const auto arena=heap_.invoke({6,{},NativeServiceKind::sysmem},ram);
            if (arena.status!=NativeServiceStatus::returned) return arena;
            if (pending_->status) store_native_word(ram,*ram_offset(pending_->status,4,ram.size()),request.arguments[0]);
            heap_.invoke({5,{pending_->scratch},NativeServiceKind::sysmem},ram);
            auto caller=pending_->caller; caller.registers[2]=0;
            pending_.reset();
            NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
        }
        if (request.kind!=NativeServiceKind::loadcore || request.selector!=20) return {};
        if (request.interrupt_context || !request.context || pending_) return {NativeServiceStatus::unavailable};
        const auto target=ram_offset(request.arguments[0],4,0x200000);
        if (!request.arguments[0] || !target || !nt_iop_native_instruction(*target))
            return {NativeServiceStatus::unmapped_code,0,request.arguments[0]};
        const Handler handler{request.arguments[0],request.global_pointer,request.arguments[1]&3U};
        if (priority_<3) {
            if (handlers_.size()==4096) return {NativeServiceStatus::unavailable};
            handlers_.push_back(handler);
            return {NativeServiceStatus::returned,1};
        }
        return begin(request,handler,1,0,request.arguments[2],ram);
    }
    // The host calls this after module entries have returned. State survives slices.
    NativeResult finish_startup(NativeEntry entry,Cpu& cpu,std::span<std::uint8_t> ram,
                                std::uint64_t budget,NativeServices& services) {
        while (budget) {
            if (!pending_) {
                while (priority_<4) {
                    while (cursor_<handlers_.size() && handlers_[cursor_].priority!=priority_) ++cursor_;
                    if (cursor_<handlers_.size()) break;
                    ++priority_; cursor_=0;
                }
                if (priority_==4) return {NativeStop::unmapped_code,completion};
                const auto handler=handlers_[cursor_];
                cpu.pc=completion;
                NativeServiceRequest request{20,{},NativeServiceKind::loadcore};
                request.context=&cpu; request.resume_pc=completion;
                const auto next=priority_==3 ? handler.address|handler.priority : handlers_.front().address|handlers_.front().priority;
                const auto reply=begin(request,handler,next,1,0,ram);
                if (reply.status!=NativeServiceStatus::called) {
                    const auto stop=reply.status==NativeServiceStatus::address_fault ? NativeStop::address_fault :
                        reply.status==NativeServiceStatus::code_write ? NativeStop::code_write : NativeStop::import_request;
                    return {stop,cpu.pc,reply.address};
                }
                ++cursor_;
                cpu.pc=handler.address; cpu.registers[28]=handler.gp;
                cpu.registers[31]=native_service_return; cpu.native_call_owner=this;
                for (unsigned i=0;i<4;++i) cpu.registers[4+i]=reply.call->arguments[i];
                cpu.load_register=0; cpu.delay_slot=false;
            } else if (pending_->owner!=&cpu) return {NativeStop::import_request,cpu.pc};
            const auto before=cpu.executed;
            const auto result=run_native(entry,cpu,ram,budget,&services);
            budget-=cpu.executed-before;
            if (result.reason==NativeStop::unmapped_code && cpu.pc==completion && !pending_) continue;
            return result;
        }
        return {NativeStop::budget,cpu.pc};
    }
    bool complete() const { return priority_==4 && !pending_; }
private:
    struct Handler { std::uint32_t address,gp,priority; };
    struct Pending { const Cpu* owner; Cpu caller; std::uint32_t scratch,status; };
    NativeHeapServices& heap_;
    std::vector<Handler> handlers_;
    std::optional<Pending> pending_;
    unsigned priority_=0;
    std::size_t cursor_=0;
    static constexpr std::uint32_t completion=0xffffffe8U;
    static NativeServiceReply writable(std::uint32_t address,std::span<const std::uint8_t> ram) {
        const auto at=ram_offset(address,4,ram.size());
        if (!at || ram.size()-*at<4 || 0x200000-*at<4) return {NativeServiceStatus::address_fault,0,address};
        if (nt_iop_native_code_word(*at)) return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply begin(const NativeServiceRequest& request,const Handler& handler,std::uint32_t next,
                             std::uint32_t delayed,std::uint32_t status,std::span<std::uint8_t> ram) {
        if (status) {
            const auto checked=writable(status,ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        const auto allocation=heap_.invoke({4,{0,4,0},NativeServiceKind::sysmem},ram);
        if (allocation.status!=NativeServiceStatus::returned) return allocation;
        if (!allocation.value) return {NativeServiceStatus::unavailable};
        const auto checked=writable(allocation.value,ram);
        if (checked.status!=NativeServiceStatus::returned) {
            heap_.invoke({5,{allocation.value},NativeServiceKind::sysmem},ram); return checked;
        }
        auto caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
        pending_=Pending{request.context,caller,allocation.value,status};
        store_native_word(ram,allocation.value,next);
        NativeServiceReply reply{NativeServiceStatus::called,handler.address};
        reply.call=NativeServiceCall{{allocation.value,delayed,0,0},handler.gp,this}; return reply;
    }
};
}
