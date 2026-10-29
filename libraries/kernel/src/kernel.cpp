#include <nt/kernel.hpp>
#include <limits>
#include <algorithm>

namespace nt::kernel {
namespace {
constexpr std::uint32_t ram_size = 32 * 1024 * 1024;
constexpr std::uint32_t stack_reserve = 0x2a0;
void require(bool condition, const char* message) {
    if (!condition) throw runtime::MemoryFault(message);
}
}
bool Kernel::service(runtime::State& state, runtime::Bus& memory) {
    const auto arg = [&](unsigned index) { return static_cast<std::uint32_t>(state.gpr[index + 4].lo); };
    const auto raw_call = static_cast<std::uint32_t>(state.gpr[3].lo);
    const auto call = (raw_call & 0x80000000U) ? 0U - raw_call : raw_call;
    switch (call) {
    case 0x02: // SetGsCrt: host device configuration, no BIOS register table.
        memory.set_gs_crt(arg(0),arg(1),arg(2));
        return true;
    case 0x70: // GsGetIMR, including the interrupt-safe negative alias.
        state.set(2,memory.load(0x12001010,8));
        return true;
    case 0x71: // GsPutIMR uses the same device semantics as direct MMIO.
        memory.store(0x12001010,8,state.gpr[4].lo);
        return true;
    case 0x6b: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7a:
        return service_sif(call,state,memory);
    case 0x4a: // SetOsdConfigParam: in-memory configuration, no firmware/NVRAM.
        osd_config_ = static_cast<std::uint32_t>(memory.load(arg(0),4));
        return true;
    case 0x4b: // GetOsdConfigParam
        memory.store(arg(0),4,osd_config_);
        return true;
    case 0x6e: case 0x6f: { // Volatile public Config2Param bytes; no firmware/NVRAM.
        const auto pointer = arg(0), size = arg(1), offset = arg(2);
        if (offset > osd_config2_.size() || size > osd_config2_.size()-offset) return false;
        const auto physical = runtime::default_ram_address(pointer);
        require(physical <= ram_size && size <= ram_size-physical,"OSD2: buffer exceeds RAM");
        for (unsigned i=0;i<size;++i) {
            if (call == 0x6e) osd_config2_[offset+i] = static_cast<std::uint8_t>(memory.load(pointer+i,1));
            else memory.store(pointer+i,1,osd_config2_[offset+i]);
        }
        return true;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x29:
    case 0x2f: case 0x30: case 0x31:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
        return service_threads(call,state,memory);
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        return service_interrupts(call,state,memory);
    case 0x64: // FlushCache: coherent RAM + instruction guards, no stale code cache.
        memory.synchronize_cache(arg(0));
        return true;
    case 0x74: { // SetSyscall(number, handler); null removes an override.
        const auto number = arg(0), address = arg(1);
        if (number >= 256 && number != 0x12c) return false;
        const auto physical = address & 0x1fffffffU;
        require(address == physical || (address >= 0x80000000U && address < 0xc0000000U), "SetSyscall: unsupported address segment");
        if (address) {
            require(physical % 4 == 0 && physical >= 4 && physical < ram_size, "SetSyscall: invalid code address");
            memory.load(physical,4);
        }
        if (number == 0x12c) {
            if (interrupt_active()) return false;
            // Publicly used kernel table alias for INTC timer-3 (line 12).
            // The alias callback uses zero argument/GP, separate from AddIntcHandler.
            const auto mask = memory.load(0x1000f010,4);
            if (!(mask & (1U << 12))) memory.store(0x1000f010,4,1U << 12);
            interrupt_handlers_[12] = physical;
        } else memory.store(syscall_table + number * 4,4,address);
        state.set(2,0);
        return true;
    }
    case 0x3c: { // SetupThread(gp, stack, stack_size, args, root)
        if (thread_.id) return false; // Reinitialization/scheduling is not implemented.
        const auto stack_size = arg(2), args = arg(3);
        const auto top = arg(1) == 0xffffffffU ? std::uint64_t(ram_size - 4096) : std::uint64_t(arg(1)) + stack_size;
        require(stack_size >= stack_reserve && top >= stack_size && top <= ram_size && top % 16 == 0,
                "SetupThread: invalid stack extent/alignment");
        require(args % 4 == 0 && args < ram_size - 7, "SetupThread: invalid argument buffer");
        // Initial launch has no application arguments: argc=0 and argv[0]=null.
        // Validate both stores before mutating the guest argument block.
        memory.load(args,4); memory.load(args + 4,4);
        memory.store(args,4,0); memory.store(args + 4,4,0);
        thread_.id = 1;
        thread_.gp = arg(0);
        thread_.stack_base = static_cast<std::uint32_t>(top - stack_size);
        thread_.stack_size = stack_size;
        thread_.root = arg(4);
        threads_.emplace(1,Thread{1,0,thread_.stack_base,stack_size,thread_.gp,0,0,0,0,{},0});
        current_thread_ = 1;
        state.set(2, static_cast<std::uint32_t>(top - stack_reserve));
        return true;
    }
    case 0x3d: { // SetupHeap(begin, size), -1 extends up to the main stack.
        if (!thread_.id) return false;
        const auto begin = arg(0), size = arg(1);
        const auto end = size == 0xffffffffU ? std::uint64_t(thread_.stack_base) : std::uint64_t(begin) + size;
        require(begin <= end && end <= thread_.stack_base, "SetupHeap: heap overlaps stack or wraps");
        thread_.heap_begin = begin;
        thread_.heap_end = static_cast<std::uint32_t>(end);
        thread_.heap_ready = true;
        state.set(2, thread_.heap_end);
        return true;
    }
    case 0x3e: // EndOfHeap
        if (!thread_.heap_ready) return false;
        state.set(2, thread_.heap_end);
        return true;
    case 0x40: { // CreateSema: count,max,initial,waiters,attributes,option
        const auto pointer = arg(0);
        require(pointer % 4 == 0 && pointer <= ram_size - 24, "CreateSema: invalid structure");
        const auto maximum = static_cast<std::uint32_t>(memory.load(pointer + 4,4));
        const auto initial = static_cast<std::uint32_t>(memory.load(pointer + 8,4));
        const auto attributes = static_cast<std::uint32_t>(memory.load(pointer + 16,4));
        const auto option = static_cast<std::uint32_t>(memory.load(pointer + 20,4));
        require(maximum > 0 && maximum <= 0x7fffffffU && initial <= maximum, "CreateSema: invalid counts");
        std::uint32_t id = 1;
        while (semaphores_.contains(id)) ++id;
        if (id > 256) { state.set(2,~std::uint64_t(0)); return true; }
        semaphores_.emplace(id, Semaphore{initial,maximum,attributes,option});
        state.set(2,id);
        return true;
    }
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: {
        const auto id = arg(0);
        const auto found = semaphores_.find(id);
        if (found == semaphores_.end()) { state.set(2,~std::uint64_t(0)); return true; }
        auto& sema = found->second;
        if (call == 0x41) {
            if (!sema.waiters.empty()) return false;
            semaphores_.erase(found);
        }
        else if (call == 0x42 || call == 0x43) {
            if (!sema.waiters.empty()) {
                auto& waiting = threads_.at(sema.waiters.front());
                waiting.status = 2; waiting.wait_semaphore = 0;
                waiting.ready_order = next_ready_order_++;
                sema.waiters.erase(sema.waiters.begin());
            } else {
                if (sema.count == sema.maximum) return false; // Overflow ABI is not established yet.
                ++sema.count;
            }
        } else if (call >= 0x44 && call <= 0x46) {
            if (!sema.count) {
                if (call == 0x44) {
                    if (!current_thread_ || !frames_.empty() || interrupt_active() || sema.attributes)
                        return false; // Non-FIFO waits and guest kernel continuations remain unsupported.
                    auto& current = threads_.at(current_thread_);
                    current.status = 4; current.wait_semaphore = id;
                    sema.waiters.push_back(current_thread_);
                    state.set(2,id);
                    return true;
                }
                state.set(2,~std::uint64_t(0)); return true;
            }
            --sema.count;
        } else {
            const auto pointer = arg(1);
            require(pointer % 4 == 0 && pointer <= ram_size - 24, "ReferSemaStatus: invalid structure");
            for (unsigned offset = 0; offset < 24; offset += 4) memory.load(pointer + offset,4);
            memory.store(pointer,4,sema.count); memory.store(pointer + 4,4,sema.maximum);
            memory.store(pointer + 12,4,sema.waiters.size());
            memory.store(pointer + 16,4,sema.attributes); memory.store(pointer + 20,4,sema.option);
        }
        state.set(2,id);
        return true;
    }
    default:
        return false;
    }
}
std::uint32_t Kernel::handler(std::uint32_t number, runtime::Bus& memory) const {
    if (number & 0x80000000U) number = 0U - number;
    if (number >= 256) return 0;
    const auto address = static_cast<std::uint32_t>(memory.load(syscall_table + number * 4,4));
    const auto physical = address & 0x1fffffffU;
    require(address == physical || (address >= 0x80000000U && address < 0xc0000000U), "Guest syscall table contains an invalid address segment");
    require(!address || (physical >= 4 && physical < ram_size && physical % 4 == 0), "Guest syscall table contains an invalid destination");
    return physical;
}
bool Kernel::enter_handler(runtime::State& state, runtime::Bus& memory) {
    const auto raw = static_cast<std::uint32_t>(state.gpr[3].lo);
    const auto address = handler(raw,memory);
    if (!address) return false;
    require(frames_.size() < 32, "Guest syscall nesting limit exceeded");
    const auto stack = static_cast<std::uint32_t>(state.gpr[29].lo);
    require(stack >= 16 && stack <= ram_size && stack % 16 == 0, "Guest handler: invalid stack");
    memory.load(address,4);
    memory.store(stack - 16,8,state.gpr[31].lo);
    frames_.push_back({state.delay_slot ? state.pending_target : state.pc + 4, stack - 16, state.cop0_status & 2U});
    state.cop0_status |= 2U;
    state.set(29,stack - 16);
    state.set(31,runtime::sx32(return_gate));
    state.set(3,(raw & 0x80000000U) ? 0U - raw : raw);
    state.pc = address;
    state.delay_slot = false;
    return true;
}
bool Kernel::return_from_handler(runtime::State& state, runtime::Bus& memory) {
    if (state.pc != return_gate || frames_.empty()) return false;
    const auto frame = frames_.back();
    require(!state.delay_slot && state.gpr[29].lo == frame.stack, "Guest handler corrupted its return frame");
    const auto ra = memory.load(frame.stack,8);
    state.set(31,ra);
    state.set(29,frame.stack + 16);
    state.pc = frame.continuation;
    state.cop0_status = (state.cop0_status & ~2U) | frame.exception_level;
    frames_.pop_back();
    return true;
}
}
