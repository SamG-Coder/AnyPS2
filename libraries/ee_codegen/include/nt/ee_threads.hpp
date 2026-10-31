#pragma once
#include <nt/ee_handlers.hpp>
#include <nt/ee_startup.hpp>
#include <map>
#include <bit>
#include <algorithm>

namespace nt::ee {
// Cooperative native continuations: all runnable entry PCs must be compiled.
class NativeThreads {
public:
    bool allow_idle=false;
    bool waiting() const { return current_ && records_.at(current_).status==4 && !has_ready(); }
    struct Result { Stop status; std::uint64_t value=0; };
    void initialize(runtime::InitialStack stack,std::uint32_t gp,std::uint32_t root) {
        root_=physical(root);
        records_.emplace(1,Record{1,0,stack.base,stack.size,gp,0,0,0,0}); current_=1;
    }
    Result invoke(std::uint32_t call,runtime::State& state,Memory& memory,CompiledCode compiled,CodeOverlap protection) {
        if (!current_) return {Stop::syscall};
        const auto arg=[&](unsigned n) { return static_cast<std::uint32_t>(state.gpr[4+n].lo); };
        const auto fail=[] { return Result{Stop::budget,~std::uint64_t(0)}; };
        if (call==0x2f) return {Stop::budget,current_};
        if (call==0x20) {
            const auto address=physical(arg(0));
            if (!range(memory,arg(0),36,4)) { memory.fault_address=arg(0); return {Stop::memory_fault}; }
            std::array<std::uint32_t,9> words{}; std::memcpy(words.data(),memory.data+address,36);
            const auto entry=physical(words[1]),stack=physical(words[2]),size=words[3];
            if (words[5]>=128 || size<0x2a0 || size%16 || !entry) return fail();
            if (!range(memory,words[1],4,4) || !range(memory,words[2],size,16)) return {Stop::invalid_state};
            if (!compiled || !compiled(entry)) { memory.fault_address=entry; return {Stop::unmapped_code}; }
            if (!protection) return {Stop::invalid_state};
            if (protection(stack,size)) { memory.fault_address=words[2]; return {Stop::code_write}; }
            std::uint32_t id=1; while (records_.contains(id)) ++id;
            if (id>=256) return fail();
            records_.emplace(id,Record{0x10,words[1],words[2],size,words[4],words[5],words[5],words[7],words[8]});
            return {Stop::budget,id};
        }
        auto id=arg(0);
        if (call==0x23 || call==0x32 || (!id && (call==0x29 || call==0x30 || call==0x31))) id=current_;
        const auto found=records_.find(id);
        if (found==records_.end()) return (call==0x30 || call==0x31) && id<256 ? Result{Stop::budget,0} : fail();
        auto& thread=found->second;
        if (call==0x21) {
            if (id==current_ || thread.status!=0x10) return fail();
            records_.erase(found); return {Stop::budget,id};
        }
        if (call==0x22) {
            if (thread.status!=0x10) return fail();
            if (!compiled || !compiled(physical(thread.entry)) || !compiled(root_)) {
                memory.fault_address=compiled && compiled(physical(thread.entry)) ? root_ : physical(thread.entry);
                return {Stop::unmapped_code};
            }
            thread.context={}; thread.context.pc=physical(thread.entry);
            thread.context.set(4,arg(1)); thread.context.set(28,signed_word(thread.gp));
            thread.context.set(29,physical(thread.stack)+thread.size-0x2a0); thread.context.set(31,signed_word(root_));
            thread.wakeups=0; thread.priority=thread.initial_priority; thread.status=2; thread.order=next_order_++;
            return {Stop::budget,id};
        }
        if (call==0x32) {
            if (thread.wakeups) { --thread.wakeups; return {Stop::budget,id}; }
            // Without an explicit event owner, preserve the caller when no
            // replacement continuation can run.
            if (!has_ready() && !allow_idle)
                return {Stop::syscall};
            thread.status=4; return {Stop::budget,id};
        }
        if (call==0x33 || call==0x34) {
            if ((id==current_ && thread.status!=4) || thread.status==0x10) return fail();
            if (thread.status==4) { thread.status=2; thread.order=next_order_++; }
            else {
                if (thread.wakeups==0x7fffffffU) return {Stop::unsupported_system};
                ++thread.wakeups;
            }
            return {Stop::budget,id};
        }
        if (call==0x35 || call==0x36) {
            const auto count=thread.wakeups; thread.wakeups=0; return {Stop::budget,count};
        }
        if (call==0x29) {
            if (arg(1)>=128) return fail();
            const auto previous=thread.priority; thread.priority=arg(1);
            return {Stop::budget,previous};
        }
        if (call==0x23) {
            if (!has_ready()) return {Stop::syscall};
            thread.status=0x10; return {Stop::budget,state.gpr[2].lo};
        }
        if (call==0x30 || call==0x31) {
            if (arg(1)) {
                if (!range(memory,arg(1),48,4)) { memory.fault_address=arg(1); return {Stop::memory_fault}; }
                const auto address=physical(arg(1));
                if (!protection) return {Stop::invalid_state};
                if (protection(address,48)) { memory.fault_address=arg(1); return {Stop::code_write}; }
                const std::array<std::uint32_t,12> words{thread.status,thread.entry,thread.stack,thread.size,thread.gp,
                    thread.initial_priority,thread.priority,thread.attributes,thread.option,thread.status==4 ? 1U : 0U,0,thread.wakeups};
                std::memcpy(memory.data+address,words.data(),48);
            }
            return {Stop::budget,thread.status};
        }
        return {Stop::syscall};
    }
    void schedule(runtime::State& state) {
        if (!current_) return;
        auto& current=records_.at(current_); auto selected=records_.end();
        for (auto i=records_.begin();i!=records_.end();++i) {
            const auto& next=i->second;
            if (next.status!=2 || (current.status==1 && next.priority>=current.priority)) continue;
            if (selected==records_.end() || next.priority<selected->second.priority ||
                (next.priority==selected->second.priority && next.order<selected->second.order)) selected=i;
        }
        if (selected==records_.end()) return;
        if (selected->first==current_) { current.status=1; return; }
        current.context=state;
        if (current.status==1) { current.status=2; current.order=next_order_++; }
        const auto executed=state.executed; const auto status=state.cop0_status;
        const auto epc=state.cop0_epc,error_epc=state.cop0_error_epc;
        state=selected->second.context; state.executed=executed; state.cop0_status=status;
        state.cop0_epc=epc; state.cop0_error_epc=error_epc;
        selected->second.status=1; current_=selected->first;
    }
private:
    bool has_ready() const {
        return std::any_of(records_.begin(),records_.end(),[](const auto& item) { return item.second.status==2; });
    }
    static std::uint64_t signed_word(std::uint32_t value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(value))); }
    static std::uint32_t physical(std::uint32_t address) { return runtime::default_ram_address(address); }
    static bool range(const Memory& memory,std::uint32_t address,std::uint32_t size,unsigned alignment) {
        const auto start=physical(address); const auto end=std::uint64_t(start)+size;
        return memory.data && !(start%alignment) && end<=memory.size && end<=0x2000000U;
    }
    struct Record {
        std::uint32_t status,entry,stack,size,gp,initial_priority,priority,attributes,option;
        runtime::State context{}; std::uint64_t order=0; std::uint32_t wakeups=0;
    };
    std::map<std::uint32_t,Record> records_;
    std::uint32_t current_=0,root_=0; std::uint64_t next_order_=0;
};
}
