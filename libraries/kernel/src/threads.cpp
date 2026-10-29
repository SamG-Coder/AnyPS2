#include <nt/kernel.hpp>
#include <algorithm>

namespace nt::kernel {
namespace {
constexpr std::uint32_t ram_size = 32 * 1024 * 1024;
constexpr std::uint32_t stack_reserve = 0x2a0;

void validate_range(std::uint32_t address, std::uint32_t size, unsigned alignment) {
    const auto physical = address & 0x1fffffffU;
    const bool mapped = address == physical || (address >= 0x80000000U && address < 0xc0000000U);
    if (!mapped || address % alignment || physical >= ram_size || size > ram_size - physical)
        throw runtime::MemoryFault("Thread service: invalid guest memory range");
}
}

bool Kernel::service_threads(std::uint32_t call, runtime::State& state, runtime::Bus& memory) {
    const auto argument = [&](unsigned index) { return static_cast<std::uint32_t>(state.gpr[index + 4].lo); };
    const auto fail = [&] { state.set(2,~std::uint64_t(0)); return true; };
    if (!current_thread_) return false;
    if (call == 0x2f) { state.set(2,current_thread_); return true; }
    if (call == 0x20) {
        const auto pointer = argument(0);
        validate_range(pointer,36,4);
        std::array<std::uint32_t,9> words{};
        for (unsigned i = 0; i < words.size(); ++i)
            words[i] = static_cast<std::uint32_t>(memory.load(pointer + i * 4,4));
        const auto entry = words[1], stack = words[2], size = words[3], priority = words[5];
        if (priority >= 128 || size < stack_reserve || size % 16) return fail();
        validate_range(entry,4,4);
        validate_range(stack,size,16);
        if (!(entry & 0x1fffffffU)) return fail();
        memory.load(entry,4);
        memory.load(stack,4);
        memory.load(stack + size - 4,4);
        std::uint32_t id = 1;
        while (threads_.contains(id)) ++id;
        if (id >= 256) return fail();
        // All descriptor and memory validation precedes mutation of kernel state.
        threads_.emplace(id,Thread{0x10,entry,stack,size,words[4],priority,priority,words[7],words[8],{},0});
        state.set(2,id);
        return true;
    }
    auto id = argument(0);
    if (call == 0x23 || (!id && (call == 0x29 || call == 0x30 || call == 0x31))) id = current_thread_;
    const auto found = threads_.find(id);
    if (found == threads_.end()) {
        if ((call == 0x30 || call == 0x31) && id < 256) { state.set(2,0); return true; }
        return fail();
    }
    auto& thread = found->second;
    if (call == 0x22) {
        if (thread.status != 0x10) return fail();
        if (!frames_.empty()) return false; // No switch across a guest kernel continuation.
        runtime::State context;
        context.pc = thread.entry & 0x1fffffffU;
        context.set(4,argument(1));
        context.set(28,runtime::sx32(thread.gp));
        context.set(29,runtime::sx32(thread.stack + thread.size - stack_reserve));
        context.set(31,runtime::sx32(thread_.root));
        thread.context = context;
        thread.priority = thread.initial_priority;
        thread.status = 2;
        thread.ready_order = next_ready_order_++;
        state.set(2,id);
        return true;
    }
    if (call == 0x29) {
        const auto priority = argument(1);
        if (priority >= 128) return fail();
        if (!frames_.empty()) return false;
        state.set(2,thread.priority);
        thread.priority = priority;
        return true;
    }
    if (call == 0x23) {
        if (!frames_.empty()) return false;
        // With no runnable successor, stop rather than invent an idle thread.
        if (std::none_of(threads_.begin(),threads_.end(),[](const auto& pair) { return pair.second.status == 2; }))
            return false;
        thread.status = 0x10;
        return true;
    }
    if (call == 0x21) {
        if (id == current_thread_ || thread.status != 0x10) return fail();
        threads_.erase(found);
        state.set(2,id);
        return true;
    }
    if (call == 0x30 || call == 0x31) {
        const auto pointer = argument(1);
        if (pointer) {
            validate_range(pointer,48,4);
            const std::array<std::uint32_t,12> words{thread.status,thread.entry,thread.stack,
                thread.size,thread.gp,thread.initial_priority,thread.priority,thread.attributes,
                thread.option,thread.wait_semaphore ? 2U : 0U,thread.wait_semaphore,0};
            for (unsigned i = 0; i < words.size(); ++i) memory.load(pointer + i * 4,4);
            for (unsigned i = 0; i < words.size(); ++i) memory.store(pointer + i * 4,4,words[i]);
        }
        state.set(2,thread.status);
        return true;
    }
    return false;
}

void Kernel::schedule(runtime::State& state) {
    if (!current_thread_ || !frames_.empty()) return;
    auto& current = threads_.at(current_thread_);
    auto selected = threads_.end();
    for (auto it = threads_.begin(); it != threads_.end(); ++it) {
        const auto& candidate = it->second;
        if (candidate.status != 2) continue;
        if (current.status == 1 && candidate.priority >= current.priority) continue;
        if (selected == threads_.end() || candidate.priority < selected->second.priority ||
            (candidate.priority == selected->second.priority && candidate.ready_order < selected->second.ready_order))
            selected = it;
    }
    if (selected == threads_.end()) return;
    current.context = state;
    if (current.status == 1) {
        current.status = 2;
        current.ready_order = next_ready_order_++;
    }
    const auto executed = state.executed;
    const auto status = state.cop0_status;
    const auto epc = state.cop0_epc, error_epc = state.cop0_error_epc;
    state = selected->second.context;
    // The instruction budget and CP0 control registers belong to the CPU,
    // not an individual thread's saved integer/floating-point context.
    state.executed = executed;
    state.cop0_status = status;
    state.cop0_epc = epc; state.cop0_error_epc = error_epc;
    selected->second.status = 1;
    current_thread_ = selected->first;
}
}
