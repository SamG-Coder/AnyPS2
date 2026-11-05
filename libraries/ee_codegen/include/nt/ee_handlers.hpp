#pragma once
#include <nt/ee_native.hpp>
#include <array>
#include <cstring>

namespace nt::ee {
using CompiledCode=bool (*)(std::uint32_t);
using CodeOverlap=bool (*)(std::uint32_t,std::uint32_t);
class SyscallHandlers {
public:
    static constexpr std::uint32_t return_gate=0xfffffff0U;
    SyscallHandlers(CompiledCode compiled,CodeOverlap protection,std::uint32_t table=0)
        : compiled_(compiled),protection_(protection),table_(table) {}
    Stop validate_view(Memory& memory) const {
        if (!table_) return Stop::budget;
        if (table_%4 || !memory.data || std::uint64_t(table_)+sizeof(targets_)>memory.size ||
            std::uint64_t(table_)+sizeof(targets_)>0x2000000U) {
            memory.fault_address=table_; return Stop::memory_fault;
        }
        if (!protection_) return Stop::invalid_state;
        if (protection_(table_,sizeof(targets_))) { memory.fault_address=table_; return Stop::code_write; }
        // The view exposes native registrations, not a writable firmware table.
        if (std::memcmp(memory.data+table_,published_.data(),sizeof(published_))!=0)
            return Stop::unsupported_system;
        return Stop::budget;
    }
    Stop install(runtime::State& state,Memory& memory) {
        const auto view=validate_view(memory);
        if (view!=Stop::budget) return view;
        const auto number=static_cast<std::uint32_t>(state.gpr[4].lo);
        const auto raw=static_cast<std::uint32_t>(state.gpr[5].lo);
        if (number>=targets_.size()) return Stop::syscall;
        const auto address=physical(raw);
        if (raw && (!address || address%4 || address>=0x2000000U)) return Stop::invalid_state;
        if (raw && (!compiled_ || !compiled_(address))) return Stop::unmapped_code;
        targets_[number]=address;
        published_[number]=raw;
        if (table_) std::memcpy(memory.data+table_+number*4,&raw,4);
        state.set(2,0); state.pc+=4; ++state.executed;
        return Stop::budget;
    }
    bool registered(std::uint32_t number) const { return number<targets_.size() && targets_[number]; }
    Stop enter(runtime::State& state,Memory& memory,std::uint32_t number) {
        if (!registered(number)) return Stop::syscall;
        const auto stack=static_cast<std::uint32_t>(state.gpr[29].lo);
        if (depth_==frames_.size() || stack<16 || stack%16 || stack>0x2000000U)
            return Stop::invalid_state;
        const auto address=stack-16;
        if (!memory.data || stack>memory.size) { memory.fault_address=address; return Stop::memory_fault; }
        if (!protection_) return Stop::invalid_state;
        if (protection_(address,8)) { memory.fault_address=address; return Stop::code_write; }
        const auto ra=state.gpr[31].lo;
        std::memcpy(memory.data+address,&ra,8);
        frames_[depth_++]={state.pc+4,address,state.cop0_status&2U};
        state.cop0_status|=2U;
        state.set(29,address); state.set(31,0xfffffffffffffff0ULL);
        state.pc=targets_[number]; ++state.executed;
        return Stop::budget;
    }
    bool active() const { return depth_!=0; }
    // A handler may leave by restoring its caller's registers itself. Interrupts
    // are delivered only with no handler active, so when one returns, any frame
    // still recorded was entered during it and can no longer return.
    void abandon_after_interrupt() { depth_=0; }
    bool returning(const runtime::State& state) const { return state.pc==return_gate && depth_; }
    Stop finish(runtime::State& state,Memory& memory) {
        if (!returning(state)) return Stop::invalid_state;
        const auto& frame=frames_[depth_-1];
        if (state.delay_slot || state.gpr[29].lo!=frame.stack) return Stop::invalid_state;
        if (!memory.data || std::uint64_t(frame.stack)+8>memory.size) {
            memory.fault_address=frame.stack; return Stop::memory_fault;
        }
        std::uint64_t ra=0; std::memcpy(&ra,memory.data+frame.stack,8);
        state.set(31,ra); state.set(29,frame.stack+16);
        state.pc=frame.continuation;
        state.cop0_status=(state.cop0_status&~2U)|frame.exception_level;
        --depth_;
        return Stop::budget;
    }
private:
    static std::uint32_t physical(std::uint32_t address) {
        return runtime::default_ram_address(address);
    }
    struct Frame { std::uint32_t continuation,stack,exception_level; };
    CompiledCode compiled_;
    CodeOverlap protection_;
    std::uint32_t table_;
    std::array<std::uint32_t,256> targets_{};
    std::array<std::uint32_t,256> published_{};
    std::array<Frame,32> frames_{};
    std::size_t depth_=0;
};
}
