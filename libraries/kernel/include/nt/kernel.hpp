#pragma once
#include <nt/runtime.hpp>
#include <map>
#include <optional>

namespace nt::kernel {
struct MainThread {
    std::uint32_t id = 0, gp = 0, stack_base = 0, stack_size = 0, root = 0;
    std::uint32_t heap_begin = 0, heap_end = 0;
    bool heap_ready = false;
};
// Initial BIOS-free HLE kernel. Only explicitly implemented services are accepted.
// Scheduling occurs at implemented kernel service boundaries, not timer interrupts.
class Kernel {
public:
    // BIOS-free EE handoff: kernel mode, interrupts enabled for INTC and DMAC.
    static void initialize_cpu(runtime::State& state) { state.cop0_status = 0x10c01; }
    bool service(runtime::State& state, runtime::Bus& memory);
    const MainThread& main_thread() const { return thread_; }
    std::uint32_t handler(std::uint32_t number, runtime::Bus& memory) const;
    bool enter_handler(runtime::State& state, runtime::Bus& memory);
    bool return_from_handler(runtime::State& state, runtime::Bus& memory);
    std::size_t handler_depth() const { return frames_.size(); }
    void schedule(runtime::State& state);
    bool enter_interrupt(runtime::State& state, runtime::Bus& memory);
    bool return_from_interrupt(runtime::State& state);
    bool interrupt_active() const { return interrupt_frame_.has_value(); }
    std::uint64_t completed_interrupts() const { return completed_interrupts_; }
    std::uint32_t interrupt_handler(unsigned line) const { return interrupt_handlers_.at(line); }
    static constexpr std::uint32_t return_gate = 0xfffffff0U;
    static constexpr std::uint32_t interrupt_return_gate = 0xffffffe0U;
    static constexpr std::uint32_t syscall_table = 0x00010000U;
private:
    struct Thread {
        std::uint32_t status = 0x10, entry = 0, stack = 0, size = 0, gp = 0;
        std::uint32_t initial_priority = 0, priority = 0, attributes = 0, option = 0;
        runtime::State context;
        std::uint64_t ready_order = 0;
        std::uint32_t wait_semaphore = 0;
    };
    std::map<std::uint32_t, Thread> threads_;
    std::uint32_t current_thread_ = 0;
    std::uint64_t next_ready_order_ = 0;
    // Volatile user configuration: OSD2, English, 4:3, stereo, UTC.
    std::uint32_t osd_config_ = (1U << 13) | (1U << 16) | (1U << 4);
    std::array<std::uint8_t,4> osd_config2_{0,0,0,1};
    std::array<std::uint32_t,3> sif_system_registers_{};
    bool service_sif(std::uint32_t call, runtime::State& state, runtime::Bus& memory);
    bool service_threads(std::uint32_t call, runtime::State& state, runtime::Bus& memory);
    struct InterruptRecord { std::uint32_t id, line, address, argument, gp; };
    std::vector<InterruptRecord> intc_records_, dmac_records_;
    struct InterruptFrame {
        runtime::State interrupted;
        std::vector<InterruptRecord> callbacks;
        std::size_t index = 0;
        std::uint32_t stack = 0;
    };
    std::optional<InterruptFrame> interrupt_frame_;
    std::uint64_t completed_interrupts_ = 0;
    void start_interrupt_callback(runtime::State& state);
    bool service_interrupts(std::uint32_t call, runtime::State& state, runtime::Bus& memory);
    struct HandlerFrame { std::uint32_t continuation, stack, exception_level; };
    std::vector<HandlerFrame> frames_;
    std::array<std::uint32_t,16> interrupt_handlers_{};
    struct Semaphore { std::uint32_t count, maximum, attributes, option; std::vector<std::uint32_t> waiters{}; };
    MainThread thread_;
    std::map<std::uint32_t, Semaphore> semaphores_;
};
struct RunResult { runtime::Result stop; std::uint64_t serviced_calls; };
RunResult run(runtime::Entry entry, runtime::State& state, runtime::Bus& memory,
              Kernel& kernel, std::uint64_t budget);
}
