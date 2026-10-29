#include <nt/kernel.hpp>

namespace nt::kernel {
bool Kernel::enter_interrupt(runtime::State& state, runtime::Bus& memory) {
    if (interrupt_active() || !frames_.empty() || state.delay_slot ||
        (state.cop0_status & 0x10007U) != 0x10001U ||
        !(state.cop0_status & memory.interrupt_lines() & 0xc00U)) return false;
    const bool dma = (state.cop0_status & memory.interrupt_lines() & 0x800U) != 0;
    const auto status_address = dma ? 0x1000e010U : 0x1000f000U;
    const auto status = static_cast<std::uint32_t>(memory.load(status_address,4));
    const auto mask = dma ? status >> 16 : static_cast<std::uint32_t>(memory.load(0x1000f010,4));
    const auto pending = status & mask & (dma ? 0x3ffU : 0xffffU);
    if (!pending) return false;
    InterruptFrame frame;
    // DMAC takes priority over INTC. DMAC channels descend; INTC lines ascend.
    // Both controllers share callback execution and interrupted-context ownership.
    const auto& records = dma ? dmac_records_ : intc_records_;
    const int count = dma ? 10 : 16;
    for (int index = 0; index < count; ++index) {
        const auto line = static_cast<unsigned>(dma ? count - 1 - index : index);
        if (!(pending & (1U << line))) continue;
        const auto before = frame.callbacks.size();
        for (const auto& record : records)
            if (record.line == line) {
                memory.load(record.address,4);
                frame.callbacks.push_back(record);
            }
        if (!dma && interrupt_handlers_[line]) {
            memory.load(interrupt_handlers_[line],4);
            frame.callbacks.push_back({0,line,interrupt_handlers_[line],0,0});
        }
        if (frame.callbacks.size() == before) return false;
    }
    const auto stack = static_cast<std::uint32_t>(state.gpr[29].lo);
    if (stack < 0x2a0 || stack > 32 * 1024 * 1024 || stack % 16)
        throw runtime::MemoryFault("Interrupt: invalid interrupted stack");
    frame.interrupted = state;
    frame.stack = stack - 0x2a0;
    memory.load(frame.stack,4);
    memory.store(status_address,4,pending); // W1C status flags, never toggle masks.
    interrupt_frame_ = std::move(frame);
    state.cop0_epc = state.pc;
    state.cop0_status = (state.cop0_status | 2U) & ~1U;
    state.delay_slot = false;
    start_interrupt_callback(state);
    return true;
}
void Kernel::start_interrupt_callback(runtime::State& state) {
    const auto& frame = *interrupt_frame_;
    const auto& callback = frame.callbacks[frame.index];
    state.pc = callback.address;
    state.set(4,callback.line);
    state.set(5,runtime::sx32(callback.argument));
    state.set(28,runtime::sx32(callback.gp));
    state.set(29,frame.stack);
    state.set(31,runtime::sx32(interrupt_return_gate));
}
bool Kernel::return_from_interrupt(runtime::State& state) {
    if (state.pc != interrupt_return_gate || !interrupt_frame_) return false;
    auto& frame = *interrupt_frame_;
    if (state.delay_slot || !frames_.empty() || state.gpr[29].lo != frame.stack)
        throw runtime::MemoryFault("Interrupt: corrupted callback return frame");
    const auto line = frame.callbacks[frame.index].line;
    ++frame.index;
    if (runtime::signed64(state.gpr[2].lo) < 0)
        while (frame.index < frame.callbacks.size() && frame.callbacks[frame.index].line == line) ++frame.index;
    if (frame.index < frame.callbacks.size()) {
        start_interrupt_callback(state);
    } else {
        const auto executed = state.executed;
        state = frame.interrupted;
        state.executed = executed;
        interrupt_frame_.reset();
        ++completed_interrupts_;
        schedule(state); // A handler may have woken a higher-priority waiter.
    }
    return true;
}
}
