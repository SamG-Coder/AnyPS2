#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_startup.hpp>
#include <nt/ee_syscalls.hpp>
#include <nt/ee_semaphores.hpp>
#include <nt/ee_handlers.hpp>
#include <nt/ee_interrupts.hpp>
#include <nt/ee_threads.hpp>
#include <nt/ee_settings.hpp>
#include <nt/ee_graphics.hpp>
#include <nt/ee_completions.hpp>
#include <nt/ee_sif_link.hpp>
#include <nt/ee_sif_outbound.hpp>
#include <cstring>
#include <atomic>
#include <optional>
#include <functional>
#include <thread>
#include <chrono>

namespace nt::ee {
struct MainThread { runtime::InitialStack stack; std::uint32_t global_pointer,root; };
class StartupServices {
public:
    // Owner pumps external native completions. A bounded idle wait preserves
    // resumable state and never consumes the compiled operation budget.
    std::function<Stop()> idle_pump;
    std::chrono::milliseconds idle_timeout{1000};
    bool waiting(const Memory& memory) const {
        return threads_.waiting() && !(memory.interrupts && memory.interrupts->active());
    }
    explicit StartupServices(CodeOverlap protected_code,CompiledCode compiled=nullptr,std::uint32_t syscall_table=0,ApplicationSettings settings={})
        : settings_(settings),handlers_(compiled,protected_code,syscall_table),protected_code_(protected_code),compiled_(compiled) {}
    bool returning(const runtime::State& state) const { return handlers_.returning(state); }
    Stop finish(runtime::State& state,Memory& memory) { return handlers_.finish(state,memory); }
    Stop finish_interrupt(runtime::State& state,Memory& memory) {
        if (handlers_.active() || !memory.interrupts) return Stop::invalid_state;
        const auto result=memory.interrupts->finish(state);
        if (result==Stop::budget && !memory.interrupts->active()) threads_.schedule(state);
        return result;
    }
    Stop interrupt(runtime::State& state,Memory& memory) {
        if (handlers_.active()) return Stop::interrupt_pending;
        return memory.interrupts ? memory.interrupts->enter(state,memory,protected_code_) : Stop::interrupt_pending;
    }
    const std::optional<MainThread>& main_thread() const { return main_; }
    const std::optional<runtime::InitialHeap>& heap() const { return heap_; }
    Stop invoke(runtime::State& state,Memory& memory) {
        if (state.delay_slot || state.pc>0xfffffffbU) return Stop::invalid_state;
        const auto view=handlers_.validate_view(memory);
        if (view!=Stop::budget) return view;
        auto number=static_cast<std::uint32_t>(state.gpr[3].lo);
        if (handlers_.registered(number)) return handlers_.enter(state,memory,number);
        if (number==0x74) {
            if (static_cast<std::uint32_t>(state.gpr[4].lo)==0x12c) {
                if (!memory.interrupts) return Stop::unsupported_system;
                const auto result=memory.interrupts->install(12,static_cast<std::uint32_t>(state.gpr[5].lo),compiled_);
                if (result!=Stop::budget) return result;
                memory.interrupts->publish(memory);
                return returned(state,0);
            }
            return handlers_.install(state,memory);
        }
        number=native_syscall_number(number);
        if (!startup_syscall(number)) return Stop::syscall;
        if (number==2) {
            if (!memory.graphics) return Stop::unsupported_system;
            const auto result=memory.graphics->configure(static_cast<std::uint32_t>(state.gpr[4].lo),
                static_cast<std::uint32_t>(state.gpr[5].lo),static_cast<std::uint32_t>(state.gpr[6].lo));
            return result==Stop::budget ? returned(state,state.gpr[2].lo) : result;
        }
        if (number==0x70) {
            if (!memory.io.read64) return Stop::unsupported_system;
            const auto result=memory.io.read64(memory.io.context,0x12001010);
            return result.status==Stop::budget ? returned(state,result.value) : result.status;
        }
        if (number==0x71) {
            if (!memory.io.write64) return Stop::unsupported_system;
            const auto result=memory.io.write64(memory.io.context,0x12001010,state.gpr[4].lo);
            return result==Stop::budget ? returned(state,state.gpr[2].lo) : result;
        }
        if (number==0x76 || number==0x77) {
            if (!memory.sif_outbound || !memory.completions) return Stop::unsupported_system;
            const auto argument=static_cast<std::uint32_t>(state.gpr[4].lo);
            const auto result=number==0x77 ? memory.sif_outbound->submit(memory,argument,static_cast<std::uint32_t>(state.gpr[5].lo)) :
                IoRead{Stop::budget,memory.sif_outbound->status(argument)};
            if (result.status!=Stop::budget) return result.status;
            return returned(state,static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(result.value))));
        }
        if (sif_control_syscall(number)) {
            if (!memory.sif_link) return Stop::unsupported_system;
            const auto result=memory.sif_link->service(number,static_cast<std::uint32_t>(state.gpr[4].lo),static_cast<std::uint32_t>(state.gpr[5].lo));
            if (result.status!=Stop::budget) return result.status;
            const auto value=(number==0x6b || number==0x78) ? state.gpr[2].lo :
                static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(result.value)));
            return returned(state,value);
        }
        if (settings_syscall(number)) {
            const auto result=settings_.invoke(number,state,memory,protected_code_);
            return result==Stop::budget ? returned(state,state.gpr[2].lo) : result;
        }
        if (thread_syscall(number)) {
            threads_.allow_idle=static_cast<bool>(idle_pump);
            const bool interrupt=memory.interrupts && memory.interrupts->active();
            if (handlers_.active() || (interrupt && number!=0x34 && number!=0x36)) return Stop::unsupported_system;
            const auto result=threads_.invoke(number,state,memory,compiled_,protected_code_);
            if (result.status!=Stop::budget) return result.status;
            returned(state,result.value);
            if (!interrupt && number!=0x34 && number!=0x36) threads_.schedule(state);
            return Stop::budget;
        }
        if (intc_syscall(number)) {
            if (!memory.interrupts) return Stop::unsupported_system;
            const auto result=memory.interrupts->service(number,state,compiled_,memory.completions);
            if (result.status!=Stop::budget) return result.status;
            memory.interrupts->publish(memory);
            if (memory.completions) memory.completions->publish(memory);
            return returned(state,result.value);
        }
        if (cache_syscall(number)) {
            if (static_cast<std::uint32_t>(state.gpr[4].lo)>3) return Stop::unsupported_system;
            // Coherent native RAM requires ordering only. Generated instructions
            // remain immutable; this cannot publish or compile copied code.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return returned(state,state.gpr[2].lo); // Public API has no return value.
        }
        if (number>=0x40 && number<=0x48) return semaphore(state,memory,number);
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
        const auto address=runtime::default_ram_address(raw);
        if (!memory.data || address%4 || std::uint64_t(address)+8>memory.size) {
            memory.fault_address=raw; return Stop::memory_fault;
        }
        if (!protected_code_) return Stop::invalid_state;
        if (protected_code_(address,8)) { memory.fault_address=raw; return Stop::code_write; }
        // Empty explicit launch context: argc=0, argv[0]=null. Validate before committing.
        std::memset(memory.data+address,0,8);
        main_=MainThread{*stack,arg(0),arg(4)};
        threads_.initialize(*stack,arg(0),arg(4));
        return returned(state,stack->pointer);
    }
private:
    Stop semaphore(runtime::State& state,Memory& memory,std::uint32_t number) {
        const auto id=static_cast<std::uint32_t>(state.gpr[4].lo);
        const auto record=number==0x40 ? nullptr : semaphores_.find(id);
        if (number!=0x40 && !record) return returned(state,~std::uint64_t(0));
        if (number==0x40 || number>=0x47) {
            const auto raw=static_cast<std::uint32_t>(state.gpr[number==0x40 ? 4 : 5].lo);
            const auto address=runtime::default_ram_address(raw);
            if (!memory.data || address%4 || std::uint64_t(address)+24>memory.size ||
                std::uint64_t(address)+24>0x2000000U) {
                memory.fault_address=raw; return Stop::memory_fault;
            }
            std::array<std::uint32_t,6> fields{};
            // The native target is little endian, matching the source ABI.
            std::memcpy(fields.data(),memory.data+address,24);
            if (number==0x40) {
                if (!fields[1] || fields[1]>0x7fffffffU || fields[2]>fields[1])
                    return Stop::invalid_state;
                const auto created=semaphores_.create({fields[2],fields[1],fields[4],fields[5]});
                return returned(state,created ? created : ~std::uint64_t(0));
            }
            if (!protected_code_) return Stop::invalid_state;
            if (protected_code_(address,24)) { memory.fault_address=raw; return Stop::code_write; }
            fields[0]=record->count; fields[1]=record->maximum;
            // The ABI leaves init_count untouched in a status query.
            fields[3]=0; fields[4]=record->attributes; fields[5]=record->option;
            std::memcpy(memory.data+address,fields.data(),24);
        } else if (number==0x41) {
            semaphores_.erase(id);
        } else if (number==0x42 || number==0x43) {
            // Overflow return semantics require a separate established contract.
            if (record->count==record->maximum) return Stop::syscall;
            semaphores_.signal(id);
        } else {
            if (!record->count) {
                if (number==0x44) return Stop::syscall;
                return returned(state,~std::uint64_t(0));
            }
            semaphores_.acquire(id);
        }
        return returned(state,id);
    }
    static Stop returned(runtime::State& state,std::uint64_t value) {
        state.set(2,value); state.pc+=4; ++state.executed;
        return Stop::budget; // A completed service consumes one operation from the runner's budget.
    }
    NativeSettings settings_;
    NativeThreads threads_;
    SyscallHandlers handlers_;
    Semaphores semaphores_;
    CodeOverlap protected_code_;
    CompiledCode compiled_;
    std::optional<MainThread> main_;
    std::optional<runtime::InitialHeap> heap_;
};
inline Stop run(Entry entry,runtime::State& state,Memory& memory,std::uint64_t budget,StartupServices& services) {
    std::optional<std::chrono::steady_clock::time_point> idle_since;
    while (budget) {
        if (memory.sif_outbound) {
            const auto transfer=memory.sif_outbound->pump(memory);
            if (transfer!=Stop::budget) return transfer;
        }
        if (memory.io.poll) memory.io.poll(memory.io.context);
        if (memory.completions) memory.completions->publish(memory);
        if (memory.interrupts) {
            if (memory.interrupts->returning(state)) {
                const auto restored=services.finish_interrupt(state,memory);
                if (restored!=Stop::budget) return restored;
            }
            memory.interrupts->publish(memory);
        }
        if (services.returning(state)) {
            const auto result=services.finish(state,memory);
            if (result!=Stop::budget) return result;
        }
        if (services.waiting(memory)) {
            if (!idle_since) idle_since=std::chrono::steady_clock::now();
            if (services.idle_pump) {
                const auto pumped=services.idle_pump();
                if (pumped!=Stop::budget) return pumped;
            }
            if (memory.io.poll) memory.io.poll(memory.io.context);
            if (memory.completions) memory.completions->publish(memory);
            if (memory.interrupts) memory.interrupts->publish(memory);
            if ((state.cop0_status&0x10007U)==0x10001U && (state.cop0_status&memory.pending_interrupts&0xff00U)) {
                const auto delivered=services.interrupt(state,memory);
                if (delivered!=Stop::budget) return delivered;
                continue;
            }
            if (std::chrono::steady_clock::now()-*idle_since>=services.idle_timeout) return Stop::thread_wait;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle_since.reset();
        const auto before=state.executed;
        const auto slice=memory.io.poll && budget>4096 ? 4096 : budget;
        const auto result=entry(&state,&memory,slice);
        const auto retired=state.executed-before;
        if (retired>slice) return Stop::invalid_state;
        budget-=retired;
        if ((result==Stop::unmapped_code || result==Stop::budget) && services.returning(state)) {
            const auto restored=services.finish(state,memory);
            if (restored!=Stop::budget) return restored;
            if (!budget) return Stop::budget;
            continue;
        }
        if (memory.interrupts && (result==Stop::unmapped_code || result==Stop::budget) && memory.interrupts->returning(state)) {
            const auto restored=services.finish_interrupt(state,memory);
            if (restored!=Stop::budget) return restored;
            if (!budget) return Stop::budget;
            continue;
        }
        if (result==Stop::interrupt_pending && budget) {
            const auto delivery=services.interrupt(state,memory);
            if (delivery!=Stop::budget) return delivery;
            continue;
        }
        if (result==Stop::budget && budget) {
            if (!retired) return Stop::invalid_state;
            continue;
        }
        if (result!=Stop::syscall || !budget) return result;
        const auto service=services.invoke(state,memory);
        if (service!=Stop::budget) return service;
        --budget;
    }
    return Stop::budget;
}
}
