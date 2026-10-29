#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_startup.hpp>
#include <nt/ee_syscalls.hpp>
#include <cstring>
#include <optional>

namespace nt::ee {
using CodeOverlap=bool (*)(std::uint32_t,std::uint32_t);
struct MainThread { runtime::InitialStack stack; std::uint32_t global_pointer,root; };
class StartupServices {
public:
    explicit StartupServices(CodeOverlap protected_code) : protected_code_(protected_code) {}
    const std::optional<MainThread>& main_thread() const { return main_; }
    const std::optional<runtime::InitialHeap>& heap() const { return heap_; }
    Stop invoke(runtime::State& state,Memory& memory) {
        if (state.delay_slot || state.pc>0xfffffffbU) return Stop::invalid_state;
        const auto number=static_cast<std::uint32_t>(state.gpr[3].lo);
        if (!startup_syscall(number)) return Stop::syscall;
        if (number==0x3e) {
            if (!heap_) return Stop::syscall;
            return returned(state,heap_->end);
        }
        if (number==0x3d) {
            if (!main_) return Stop::syscall;
            const auto heap=runtime::initial_heap(static_cast<std::uint32_t>(state.gpr[4].lo),
                static_cast<std::uint32_t>(state.gpr[5].lo),main_->stack.base);
            if (!heap || heap->end>memory.size) return Stop::invalid_state;
            heap_=heap;
            return returned(state,heap->end);
        }
        if (main_) return Stop::syscall;
        const auto arg=[&](unsigned n) { return static_cast<std::uint32_t>(state.gpr[4+n].lo); };
        const auto stack=runtime::initial_stack(arg(1),arg(2),memory.size);
        if (!stack) return Stop::invalid_state;
        const auto raw=arg(3);
        const auto address=raw>=0x80000000U && raw<0xc0000000U ? raw&0x1fffffffU : raw;
        if (!memory.data || address%4 || std::uint64_t(address)+8>memory.size) {
            memory.fault_address=raw; return Stop::memory_fault;
        }
        if (!protected_code_) return Stop::invalid_state;
        if (protected_code_(address,8)) { memory.fault_address=raw; return Stop::code_write; }
        // Empty explicit launch context: argc=0, argv[0]=null. Validate before committing.
        std::memset(memory.data+address,0,8);
        main_=MainThread{*stack,arg(0),arg(4)};
        return returned(state,stack->pointer);
    }
private:
    static Stop returned(runtime::State& state,std::uint32_t value) {
        state.set(2,value); state.pc+=4; ++state.executed;
        return Stop::budget; // A completed service consumes one operation from the runner's budget.
    }
    CodeOverlap protected_code_;
    std::optional<MainThread> main_;
    std::optional<runtime::InitialHeap> heap_;
};
inline Stop run(Entry entry,runtime::State& state,Memory& memory,std::uint64_t budget,StartupServices& services) {
    while (budget) {
        const auto before=state.executed;
        const auto result=entry(&state,&memory,budget);
        const auto retired=state.executed-before;
        if (retired>budget) return Stop::invalid_state;
        budget-=retired;
        if (result!=Stop::syscall || !budget) return result;
        const auto service=services.invoke(state,memory);
        if (service!=Stop::budget) return service;
        --budget;
    }
    return Stop::budget;
}
}
