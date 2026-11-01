#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
// Serialized native event domain. Device integrations post causes explicitly;
// no MMIO registers, exception vectors or firmware instructions are installed.
class NativeInterruptServices final : public NativeServices {
public:
    explicit NativeInterruptServices(std::uint32_t stack_top,NativeCallbackSource* callbacks=nullptr) : stack_top_(stack_top), callbacks_(callbacks) {
        if (stack_top<16 || stack_top>0x200000 || stack_top%16) throw std::invalid_argument("Invalid native callback stack");
    }
    bool delivery_enabled() const { return enabled_; }
    void clear_pending(unsigned irq) {
        if (!(irq<=45 || irq==62 || irq==63)) throw std::out_of_range("Invalid native interrupt cause");
        lines_[irq].pending=false;
    }
    void post(unsigned irq) {
        if (!(irq<=45 || irq==62 || irq==63)) throw std::out_of_range("Invalid native interrupt cause");
        lines_[irq].pending=true;
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) {
            if (!invocation_ || !request.context || invocation_->owner!=request.context ||
                request.context->native_call_owner!=this) return {};
            auto caller=invocation_->caller; caller.registers[2]=request.arguments[0];
            enabled_=invocation_->enabled; invocation_.reset();
            NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
        }
        if (request.kind!=NativeServiceKind::intrman) return {};
        if (request.selector==14) {
            if (invocation_ || !request.context || request.interrupt_context) return {NativeServiceStatus::unavailable};
            const auto target=ram_offset(request.arguments[0],4,0x200000);
            if (!request.arguments[0] || !target || !nt_iop_native_instruction(*target))
                return {NativeServiceStatus::unmapped_code,0,request.arguments[0]};
            auto caller=*request.context;
            if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
            caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
            invocation_=Invocation{request.context,caller,enabled_}; enabled_=false;
            NativeServiceReply reply{NativeServiceStatus::called,request.arguments[0]};
            reply.call=NativeServiceCall{{request.arguments[1],request.arguments[2],request.arguments[3],request.arguments[3]},request.global_pointer,this};
            return reply;
        }
        const auto& a=request.arguments;
        const auto returned=[](std::int32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        const auto output=[&](std::uint32_t pointer,std::uint32_t value) {
            if (!pointer) return returned();
            const auto offset=ram_offset(pointer,4,ram.size());
            if (!offset) return NativeServiceReply{NativeServiceStatus::address_fault,0,pointer};
            if (nt_iop_native_code_word(*offset)) return NativeServiceReply{NativeServiceStatus::code_write,0,pointer};
            store_native_word(ram,*offset,value); return returned();
        };
        if (request.selector==4 || request.selector==5) {
            if (saved_) return returned(-100);
            if (!(a[0]<=45 || a[0]==62 || a[0]==63)) return returned(-101);
            auto& line=lines_[a[0]];
            if (request.selector==5) {
                if (!line.handler) return returned(-105);
                line.handler.reset(); return returned();
            }
            if (a[1]>2) return {};
            const auto target=ram_offset(a[2],4,0x200000);
            if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,a[2]};
            if (line.handler) return returned(-104);
            line.handler=Handler{a[2],a[3]}; return returned();
        }
        if (request.selector==6 || request.selector==7) {
            const auto irq=a[0]&255;
            if ((a[0]&~0x3ffU) || !(irq<32 || (irq>=32 && irq<=38) || (irq>=40 && irq<=45))) return returned(-101);
            auto& line=lines_[irq];
            if (request.selector==6) { line.enabled=true; line.token=irq<32 ? irq : a[0]; return returned(); }
            const auto result=line.enabled ? 0 : -103;
            const auto written=output(a[1],line.enabled ? line.token : static_cast<std::uint32_t>(-103));
            if (written.status!=NativeServiceStatus::returned) return written;
            line.enabled=false; return returned(result);
        }
        switch (request.selector) {
        case 8: { const auto old=enabled_; enabled_=false; return returned(old ? 0 : -102); }
        case 9: enabled_=true; return returned();
        case 17: {
            const auto old=enabled_;
            const auto written=output(a[0],old ? 1 : 0);
            if (written.status!=NativeServiceStatus::returned) return written;
            enabled_=false; return returned(old ? 0 : -102);
        }
        case 18:
            if (a[0]>1) return {};
            enabled_=a[0]!=0; return returned();
        case 23: return returned(saved_ ? 1 : 0);
        default: return {};
        }
    }
    NativeResult run(NativeEntry entry,Cpu& cpu,std::span<std::uint8_t> ram,std::uint64_t budget,NativeServices& services) {
        if (!budget) return {NativeStop::budget,cpu.pc};
        if (stack_top_>ram.size()) return {NativeStop::address_fault,cpu.pc,stack_top_};
        while (budget) {
            if (!saved_ && !invocation_ && enabled_ && !cpu.delay_slot) {
                for (unsigned irq=0;irq<lines_.size();++irq) {
                    auto& line=lines_[irq];
                    if (!line.pending || !line.enabled || !line.handler) continue;
                    active_=irq; source_active_=false;
                    line.pending=false; line.enabled=false;
                    enter(cpu,line.handler->address,line.handler->argument);
                    break;
                }
                if (!saved_ && callbacks_) {
                    const auto callback=callbacks_->take();
                    if (auto failed=callbacks_->failure()) { failed->pc=cpu.pc; return *failed; }
                    if (callback) {
                        source_active_=true; enter(cpu,callback->address,callback->argument);
                        if (callback->second_argument) cpu.registers[5]=*callback->second_argument;
                    }
                }
            }
            const auto before=cpu.executed;
            const auto slice=std::min<std::uint64_t>(budget,cpu.delay_slot ? 1 : 256);
            const auto stop=run_native(entry,cpu,ram,slice,&services);
            budget-=cpu.executed-before;
            if (saved_ && stop.reason==NativeStop::unmapped_code && cpu.pc==return_address && !cpu.delay_slot) {
                const auto executed=cpu.executed;
                if (source_active_) callbacks_->finish(cpu.registers[2]);
                else lines_[active_].enabled=cpu.registers[2]!=0;
                cpu=*saved_; cpu.executed=executed; saved_.reset(); enabled_=prior_enabled_;
                if (reschedule_) { reschedule_=false; return {NativeStop::thread_yield,cpu.pc}; }
                continue;
            }
            if (saved_ && stop.reason==NativeStop::thread_yield) { reschedule_=true; continue; }
            if (stop.reason!=NativeStop::budget) return stop;
        }
        return {NativeStop::budget,cpu.pc};
    }
private:
    void enter(Cpu& cpu,std::uint32_t address,std::uint32_t argument) {
        saved_=cpu; prior_enabled_=enabled_; enabled_=false;
        cpu.pc=address; cpu.registers[4]=argument;
        cpu.registers[29]=stack_top_; cpu.registers[31]=return_address;
        cpu.load_register=0; cpu.delay_slot=false; cpu.interrupt_depth=1;
    }
    struct Invocation { const Cpu* owner; Cpu caller; bool enabled; };
    std::optional<Invocation> invocation_;
    struct Handler { std::uint32_t address, argument; };
    struct Line { std::optional<Handler> handler; bool enabled=false, pending=false; std::uint32_t token=0; };
    std::array<Line,64> lines_{};
    std::optional<Cpu> saved_;
    unsigned active_=0;
    bool enabled_=false, prior_enabled_=false;
    std::uint32_t stack_top_;
    NativeCallbackSource* callbacks_;
    bool source_active_=false;
    bool reschedule_=false;
    static constexpr std::uint32_t return_address=0xfffffff8;
};
}
