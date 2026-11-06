#pragma once
#include <nt/ee_handlers.hpp>
#include <nt/ee_exception.hpp>
#include <nt/ee_completions.hpp>
#include <optional>
#include <bit>
#include <vector>
#include <algorithm>

namespace nt::ee {
// Cooperative native event ownership. Producers raise lines between compiled
// execution slices; this class neither clocks devices nor manufactures events.
class NativeInterrupts {
public:
    static constexpr std::uint32_t return_gate=0xffffffe0U;
    void* observation_context=nullptr;
    void (*observe_delivery)(void*,bool,unsigned,const runtime::State&)=nullptr;
    void raise(unsigned line) { if (line<16) pending_|=1U<<line; }
    // SetVSyncFlag targets, written by the next vertical blank. A caller may
    // register stack storage, so each registration is delivered once.
    struct DisplayFlag { std::uint32_t flag,status; };
    void set_display_flag(std::optional<DisplayFlag> target) { display_flag_=target; }
    std::optional<DisplayFlag> take_display_flag() { const auto target=display_flag_; display_flag_.reset(); return target; }
    void raise_sbus(unsigned cause) {
        if (cause<16) { sbus_pending_|=1U<<cause; raise(1); }
    }
    void publish(Memory& memory) const {
        memory.pending_interrupts=(memory.pending_interrupts&~0x400U)|((pending_&enabled_) ? 0x400U : 0U);
    }
    IoRead read(std::uint32_t address) const {
        // A slow uncached read of the status register can observe a cause before
        // the processor takes its interrupt, and programs poll while a handler
        // is enabled. Native delivery is immediate, so a delivered cause stays
        // visible to the next read instead of making such a poll unbounded.
        if (address==0x1000f000) {
            const auto status=pending_|delivered_;
            delivered_=0;
            return {Stop::budget,status};
        }
        if (address==0x1000f010) return {Stop::budget,enabled_};
        return {Stop::memory_fault};
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address==0x1000f000) { pending_&=~(value&65535); delivered_&=~(value&65535); }
        else if (address==0x1000f010) enabled_^=value&65535;
        else return Stop::memory_fault;
        return Stop::budget;
    }
    Stop install(unsigned line,std::uint32_t raw,CompiledCode compiled) {
        if (line>=handlers_.size() || interrupted_) return Stop::unsupported_system;
        const auto address=runtime::default_ram_address(raw);
        if (raw && (!address || address%4 || address>=0x2000000U)) return Stop::invalid_state;
        if (raw && (!compiled || !compiled(address))) return Stop::unmapped_code;
        handlers_[line]=address; enabled_|=1U<<line;
        return Stop::budget;
    }
    struct ServiceResult { Stop status; std::uint64_t value=0; };
    ServiceResult service(std::uint32_t number,const runtime::State& state,CompiledCode compiled,NativeCompletions* completions=nullptr) {
        const bool dma=number==0x12 || number==0x13 || number==0x16 || number==0x17 || number==0xffffffe4U || number==0xffffffe3U;
        if (dma && !completions) return {Stop::unsupported_system};
        const auto line=static_cast<std::uint32_t>(state.gpr[4].lo);
        if (line>=(dma ? 10U : 16U)) return {Stop::budget,~std::uint64_t(0)};
        if (number==0x0a || number==0x0b) {
            if (interrupted_) return {Stop::unsupported_system};
            if (number==0x0b) {
                sbus_[line]={};
                return {Stop::budget,0};
            }
            const auto raw=static_cast<std::uint32_t>(state.gpr[5].lo);
            const auto address=runtime::default_ram_address(raw);
            if (!address || address%4 || address>=0x2000000U) return {Stop::invalid_state};
            if (!compiled || !compiled(address)) return {Stop::unmapped_code};
            sbus_[line]={0,line,address,0,static_cast<std::uint32_t>(state.gpr[28].lo),false,true};
            return {Stop::budget,0};
        }
        if (number==0x10 || number==0x11 || number==0x12 || number==0x13) {
            if (interrupted_) return {Stop::unsupported_system};
            if (number==0x11 || number==0x13) {
                const auto id=static_cast<std::uint32_t>(state.gpr[5].lo);
                const auto record=std::find_if(records_.begin(),records_.end(),[&](const auto& r) { return r.id==id && r.line==line && r.dma==dma; });
                if (record==records_.end()) return {Stop::budget,~std::uint64_t(0)};
                records_.erase(record);
                return {Stop::budget,static_cast<std::uint64_t>(std::count_if(records_.begin(),records_.end(),[&](const auto& r) { return r.line==line && r.dma==dma; }))};
            }
            const auto raw=static_cast<std::uint32_t>(state.gpr[5].lo);
            const auto address=runtime::default_ram_address(raw);
            if (!address || address%4 || address>=0x2000000U) return {Stop::invalid_state};
            if (!compiled || !compiled(address)) return {Stop::unmapped_code};
            const auto next=static_cast<std::uint32_t>(state.gpr[6].lo);
            auto position=records_.begin();
            if (next==0xffffffffU) position=records_.end();
            else if (next) {
                position=std::find_if(records_.begin(),records_.end(),[&](const auto& r) { return r.id==next && r.dma==dma; });
                if (position==records_.end()) return {Stop::budget,~std::uint64_t(0)};
            }
            if (std::count_if(records_.begin(),records_.end(),[&](const auto& r) { return r.dma==dma; })==64) return {Stop::budget,~std::uint64_t(0)};
            std::uint32_t id=1;
            while (std::any_of(records_.begin(),records_.end(),[&](const auto& r) { return r.id==id && r.dma==dma; })) ++id;
            records_.insert(position,{id,line,address,static_cast<std::uint32_t>(state.gpr[7].lo),static_cast<std::uint32_t>(state.gpr[28].lo),dma});
            return {Stop::budget,id};
        }
        if (dma) return {Stop::budget,completions->enable(line,number==0x16 || number==0xffffffe4U) ? 1U : 0U};
        if (number!=0x14 && number!=0x15 && number!=0xffffffe6U && number!=0xffffffe5U) return {Stop::unsupported_system};
        const bool enable=number==0x14 || number==0xffffffe6U;
        const bool previous=(enabled_&(1U<<line))!=0;
        if (enable) enabled_|=1U<<line; else enabled_&=~(1U<<line);
        return {Stop::budget,previous!=enable ? 1U : 0U};
    }
    // Exec replaces the process's INTC callbacks and masks delivery. Pending
    // producer events and DMA ownership are not silently acknowledged/reset.
    void replace_image() {
        enabled_=0; handlers_={}; sbus_={};
        std::erase_if(records_,[](const auto& record) { return !record.dma; });
    }
    bool active() const { return interrupted_.has_value(); }
    // True while a callback delivered for this INTC line (not a DMAC channel) runs.
    bool handling(unsigned line) const { return interrupted_ && active_line_==int(line); }
    bool returning(const runtime::State& state) const { return interrupted_ && state.pc==return_gate; }
    Stop enter(runtime::State& state,Memory& memory,CodeOverlap protection) {
        if (interrupted_ || state.delay_slot || (state.cop0_status&0x10007U)!=0x10001U)
            return Stop::interrupt_pending;
        const bool dma=(state.cop0_status&memory.pending_interrupts&0x800U)!=0;
        if (!dma && !(state.cop0_status&0x400U)) return Stop::interrupt_pending;
        const auto ready=dma ? (memory.completions ? memory.completions->ready() : 0U) : pending_&enabled_;
        if (!ready) return Stop::interrupt_pending;
        const auto line=static_cast<unsigned>(std::countr_zero(ready));
        std::vector<Record> callbacks;
        for (const auto& record:records_) if (record.line==line && record.dma==dma) callbacks.push_back(record);
        if (!dma && handlers_[line]) callbacks.push_back({0,line,handlers_[line],0,0});
        if (!dma && line==1) {
            for (unsigned cause=0;cause<16;++cause)
                if ((sbus_pending_&(1U<<cause)) && sbus_[cause].address) callbacks.push_back(sbus_[cause]);
        }
        if (callbacks.empty()) return Stop::interrupt_pending;
        const auto stack=static_cast<std::uint32_t>(state.gpr[29].lo);
        if (stack<0x2a0 || stack%16 || stack>0x2000000U) return Stop::invalid_state;
        if (!memory.data || stack>memory.size) { memory.fault_address=stack; return Stop::memory_fault; }
        if (!protection) return Stop::invalid_state;
        if (protection(stack-0x2a0,0x2a0)) { memory.fault_address=stack-0x2a0; return Stop::code_write; }
        if (observe_delivery) observe_delivery(observation_context,dma,line,state);
        interrupted_=state; stack_=stack-0x2a0; active_line_=dma ? -1 : int(line);
        record_exception(state,memory,ExceptionCode::interrupt);
        state.cop0_epc=state.pc; state.cop0_status=(state.cop0_status|2U)&~1U;
        callbacks_=std::move(callbacks); callback_=0;
        start(state);
        if (dma) { memory.completions->acknowledge(1U<<line); memory.completions->publish(memory); }
        else { pending_&=~(1U<<line); delivered_|=1U<<line; if (line==1) sbus_pending_=0; publish(memory); }
        return Stop::budget;
    }
    Stop finish(runtime::State& state) {
        if (!returning(state) || state.delay_slot || state.gpr[29].lo!=stack_) return Stop::invalid_state;
        if ((callbacks_[callback_].sbus || !(state.gpr[2].lo>>63)) && ++callback_<callbacks_.size()) {
            start(state); return Stop::budget;
        }
        const auto executed=state.executed;
        const auto epc=state.cop0_epc;
        state=*interrupted_; state.executed=executed; state.cop0_epc=epc; interrupted_.reset(); callbacks_.clear();
        return Stop::budget;
    }
private:
    struct Record { std::uint32_t id,line,address,argument,gp; bool dma=false,sbus=false; };
    void start(runtime::State& state) const {
        const auto& record=callbacks_[callback_];
        const auto signed_word=[](std::uint32_t value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(value))); };
        state.pc=record.address; state.set(4,record.line); state.set(5,signed_word(record.argument));
        state.set(28,signed_word(record.gp)); state.set(29,stack_); state.set(31,0xffffffffffffffe0ULL);
        state.cop0_status=(state.cop0_status|2U)&~1U;
    }
    std::array<Record,16> sbus_{};
    std::uint32_t sbus_pending_=0;
    std::vector<Record> records_,callbacks_;
    std::size_t callback_=0;
    std::array<std::uint32_t,16> handlers_{};
    std::uint32_t pending_=0,enabled_=0,stack_=0;
    int active_line_=-1;
    mutable std::uint32_t delivered_=0;
    std::optional<runtime::State> interrupted_;
    std::optional<DisplayFlag> display_flag_;
};
}
