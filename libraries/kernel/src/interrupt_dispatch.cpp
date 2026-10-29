#include <nt/kernel.hpp>

namespace nt::kernel {
bool Kernel::enter_interrupt(runtime::State& state, runtime::Bus& memory) {
    if (interrupt_active() || !frames_.empty() || state.delay_slot ||
        (state.cop0_status & 0x10007U) != 0x10001U ||
        !(state.cop0_status & memory.interrupt_lines() & 0x800U)) return false;
    const auto status = static_cast<std::uint32_t>(memory.load(0x1000e010,4));
    const auto pending = status & (status >> 16) & 0x3ff;
    if (!pending) return false;
    InterruptFrame frame;
    // DMAC dispatches higher channel numbers first, preserving registration order
    // within a channel. Unknown channels remain pending rather than disappearing.
    for (int line = 9; line >= 0; --line) {
        if (!(pending & (1U << line))) continue;
        const auto before = frame.callbacks.size();
        for (const auto& record : dmac_records_)
            if (record.line == static_cast<unsigned>(line)) {
                memory.load(record.address,4);
                frame.callbacks.push_back(record);
            }
        if (frame.callbacks.size() == before) return false;
    }
    const auto stack = static_cast<std::uint32_t>(state.gpr[29].lo);
    if (stack < 0x2a0 || stack > 32 * 1024 * 1024 || stack % 16)
        throw runtime::MemoryFault("DMA interrupt: invalid interrupted stack");
    frame.interrupted = state;
    frame.stack = stack - 0x2a0;
    memory.load(frame.stack,4);
    memory.store(0x1000e010,4,pending); // W1C completion flags, never toggle masks.
    interrupt_frame_ = std::move(frame);
    state.cop0_epc = state.pc;
    state.cop0_status |= 2U;
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
        throw runtime::MemoryFault("DMA interrupt: corrupted callback return frame");
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
