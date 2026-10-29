#include <nt/kernel.hpp>

namespace nt::kernel {
RunResult run(runtime::Entry entry, runtime::State& state, runtime::Bus& memory,
              Kernel& kernel, std::uint64_t budget) {
    std::uint64_t calls = 0;
    while (budget) {
        try {
            kernel.return_from_handler(state,memory);
            kernel.return_from_interrupt(state);
            kernel.enter_interrupt(state,memory);
        } catch (const runtime::MemoryFault& error) {
            return {{runtime::Stop::memory_fault,state.pc,0,error.what()},calls};
        }
        const auto before = state.executed;
        const auto stop = entry(state,memory,budget);
        const auto used = state.executed - before;
        if (used > budget) throw std::runtime_error("Generated code exceeded its instruction budget");
        budget -= used;
        if (stop.reason == runtime::Stop::unmapped_code && state.pc == Kernel::interrupt_return_gate && kernel.interrupt_active()) {
            try { kernel.return_from_interrupt(state); }
            catch (const runtime::MemoryFault& error) { return {{runtime::Stop::memory_fault,state.pc,0,error.what()},calls}; }
            continue;
        }
        if (stop.reason == runtime::Stop::interrupt_pending && budget) {
            try { if (kernel.enter_interrupt(state,memory)) continue; }
            catch (const runtime::MemoryFault& error) { return {{runtime::Stop::memory_fault,state.pc,0,error.what()},calls}; }
        }
        if (stop.reason == runtime::Stop::unmapped_code && state.pc == Kernel::return_gate && kernel.handler_depth()) {
            try { kernel.return_from_handler(state,memory); }
            catch (const runtime::MemoryFault& error) { return {{runtime::Stop::memory_fault,state.pc,0,error.what()},calls}; }
            continue;
        }
        if (stop.reason != runtime::Stop::syscall || !budget) return {stop,calls};
        try {
            if (kernel.enter_handler(state,memory)) {
                ++calls; ++state.executed; --budget;
                memory.advance_cycles(1);
                continue;
            }
            if (!kernel.service(state,memory)) return {stop,calls};
        } catch (const runtime::MemoryFault& error) {
            return {{runtime::Stop::memory_fault,state.pc,stop.word,error.what()},calls};
        }
        ++calls;
        ++state.executed;
        try { memory.advance_cycles(1); }
        catch (const runtime::MemoryFault& error) { return {{runtime::Stop::memory_fault,state.pc,stop.word,error.what()},calls}; }
        --budget;
        state.pc = state.delay_slot ? state.pending_target : state.pc + 4;
        state.delay_slot = false;
        kernel.schedule(state);
    }
    return {{runtime::Stop::budget,state.pc,0},calls};
}
}
