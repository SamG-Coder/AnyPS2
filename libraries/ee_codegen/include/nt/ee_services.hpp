#pragma once
#include <cstring>
#include <optional>
#include <string_view>
#include <nt/ee_native.hpp>
#include <nt/ee_startup.hpp>
#include <nt/ee_syscalls.hpp>
#include <nt/ee_semaphores.hpp>
#include <nt/ee_handlers.hpp>
#include <nt/ee_interrupts.hpp>
#include <nt/ee_threads.hpp>
#include <nt/ee_launch.hpp>
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
#include <map>

namespace nt::ee {
struct MainThread { runtime::InitialStack stack; std::uint32_t global_pointer,root; };
struct ExecutionProfile {
    std::uint64_t entry_ns=0,service_ns=0,idle_ns=0,entries=0,operations=0;
    std::uint64_t longest_entry_ns=0;
    std::uint64_t display_waits=0,job_waits=0,display_sleep_ns=0,job_sleep_ns=0,job_spin_ns=0;
    struct Site { std::uint64_t nanoseconds=0,operations=0,calls=0; };
    std::map<std::uint32_t,Site> sites;
    // Native-job waits by program counter and stop reason (pc<<8|reason):
    // calls counts waits and nanoseconds their host sleep.
    std::map<std::uint64_t,Site> job_wait_sites;
    // Idle spans (every thread waiting) by the interrupts that ended them:
    // enabled pending INTC lines, plus enabled pending DMAC channels << 16.
    std::map<std::uint64_t,Site> idle_wakes;
};
class StartupServices {
public:
    // Optional wall-time accounting. Compiled entry time includes native calls.
    ExecutionProfile* profile=nullptr;
    // Owner pumps external native completions. A bounded idle wait preserves
    // resumable state and never consumes the compiled operation budget.
    std::function<Stop()> idle_pump;
    // The program's debug console text. Without this, the call stops as unsupported.
    std::function<void(std::string_view)> console_output;
    std::function<Stop()> poll_native_jobs;
    std::function<bool()> native_jobs_pending;
    std::function<std::uint64_t()> native_jobs_completed;
    // Called while no program thread can run, before the idle loop sleeps.
    std::function<void()> program_waiting;
    // Validate and activate an AOT image after arguments/ownership are checked.
    // Failure must preserve image identity and all application state.
    std::function<Stop(Memory&,std::uint32_t)> publish_image;
    // Owner-thread UI/cancellation hook, including during native idle waits.
    std::function<bool()> stop_requested;
    std::chrono::milliseconds idle_timeout{1000};
    bool waiting(const Memory& memory) const {
        return threads_.waiting() && !(memory.interrupts && memory.interrupts->active());
    }
    bool foreground_waiting() const { return threads_.waiting(); }
    explicit StartupServices(CodeOverlap protected_code,CompiledCode compiled=nullptr,std::uint32_t syscall_table=0,ApplicationSettings settings={})
        : settings_(settings),handlers_(compiled,protected_code,syscall_table),protected_code_(protected_code),compiled_(compiled) {}
    bool returning(const runtime::State& state) const { return handlers_.returning(state); }
    Stop finish(runtime::State& state,Memory& memory) { return handlers_.finish(state,memory); }
    Stop finish_interrupt(runtime::State& state,Memory& memory) {
        if (!memory.interrupts) return Stop::invalid_state;
        if (handlers_.active()) {
            if (!memory.interrupts->returning(state)) return Stop::invalid_state;
            handlers_.abandon_after_interrupt();
        }
        const auto result=memory.interrupts->finish(state);
        if (result==Stop::budget && !memory.interrupts->active()) threads_.schedule(state);
        return result;
    }
    Stop interrupt(runtime::State& state,Memory& memory) {
        if (handlers_.active()) return Stop::interrupt_pending;
        return memory.interrupts ? memory.interrupts->enter(state,memory,protected_code_) : Stop::interrupt_pending;
    }
    const std::optional<MainThread>& main_thread() const { return main_; }
    std::optional<NativeThreads::Current> current_thread() const { return threads_.current(); }
    auto thread_snapshot(const runtime::State& state) const { return threads_.snapshot(state); }
    const std::optional<runtime::InitialHeap>& heap() const { return heap_; }
    Stop invoke(runtime::State& state,Memory& memory) {
        if (state.delay_slot || state.pc>0xfffffffbU) return Stop::invalid_state;
        const auto view=handlers_.validate_view(memory);
        if (view!=Stop::budget) return view;
        auto number=static_cast<std::uint32_t>(state.gpr[3].lo);
        // A negative number is the interrupt-context form of the same table slot.
        const auto slot=static_cast<std::int32_t>(number)<0 ? 0U-number : number;
        if (handlers_.registered(slot)) return handlers_.enter(state,memory,slot);
        if (number==0x7f) {
            if (!memory.data || !memory.size || memory.size>0x2000000U) return Stop::invalid_state;
            return returned(state,memory.size);
        }
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
        // Deci2Call kputs: a1 points to a word holding the text's address.
        if (number==0x7c && static_cast<std::uint32_t>(state.gpr[4].lo)==0x10 && console_output) {
            const auto read_text=[&]() -> std::optional<std::string_view> {
                const auto holder=runtime::default_ram_address(static_cast<std::uint32_t>(state.gpr[5].lo));
                if (!memory.data || holder%4 || std::uint64_t(holder)+4>memory.size) return {};
                std::uint32_t pointer=0; std::memcpy(&pointer,memory.data+holder,4);
                const auto text=runtime::default_ram_address(pointer);
                if (text>=memory.size) return {};
                const auto limit=std::min<std::uint64_t>(memory.size-text,4096);
                const auto* begin=reinterpret_cast<const char*>(memory.data+text);
                const auto length=strnlen(begin,limit);
                if (length==limit) return {};
                return std::string_view(begin,length);
            };
            const auto text=read_text();
            if (!text) { memory.fault_address=static_cast<std::uint32_t>(state.gpr[5].lo); return Stop::memory_fault; }
            console_output(*text);
            return returned(state,0);
        }
        if (!startup_syscall(number)) return Stop::syscall;
        if (number==7) return replace_image(state,memory);
        if (number==2) {
            if (!memory.graphics) return Stop::unsupported_system;
            const auto result=memory.graphics->configure(static_cast<std::uint32_t>(state.gpr[4].lo),
                static_cast<std::uint32_t>(state.gpr[5].lo),static_cast<std::uint32_t>(state.gpr[6].lo));
            return result==Stop::budget ? returned(state,state.gpr[2].lo) : result;
        }
        if (number==0x73) {
            if (!memory.interrupts) return Stop::unsupported_system;
            const auto flag=static_cast<std::uint32_t>(state.gpr[4].lo),status=static_cast<std::uint32_t>(state.gpr[5].lo);
            if (!flag && !status) { memory.interrupts->set_display_flag({}); return returned(state,state.gpr[2].lo); }
            const auto physical_flag=runtime::default_ram_address(flag),physical_status=runtime::default_ram_address(status);
            if (flag%4 || status%8) return Stop::invalid_state;
            if (!memory.data || std::uint64_t(physical_flag)+4>memory.size) { memory.fault_address=flag; return Stop::memory_fault; }
            if (std::uint64_t(physical_status)+8>memory.size) { memory.fault_address=status; return Stop::memory_fault; }
            if (!protected_code_) return Stop::invalid_state;
            if (protected_code_(physical_flag,4)) { memory.fault_address=flag; return Stop::code_write; }
            if (protected_code_(physical_status,8)) { memory.fault_address=status; return Stop::code_write; }
            memory.interrupts->set_display_flag(NativeInterrupts::DisplayFlag{physical_flag,physical_status});
            return returned(state,state.gpr[2].lo);
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
            // Interrupt-context forms: iGetThreadId, iWakeupThread, iCancelWakeupThread
            // and iReleaseWaitThread. They never reschedule from the handler.
            const bool interrupt_safe=number==0x2f || number==0x34 || number==0x36 || number==0x2e;
            // Handlers (interrupt or registered callbacks) may use only these forms.
            const bool handler=interrupt || handlers_.active();
            if (handler && !interrupt_safe) return Stop::unsupported_system;
            const auto result=threads_.invoke(number,state,memory,compiled_,protected_code_);
            if (result.status!=Stop::budget) return result.status;
            returned(state,result.value);
            if (!handler && !interrupt_safe) threads_.schedule(state);
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
        const auto arguments=launch_.write(memory,arg(3),protected_code_);
        if (arguments!=Stop::budget) return arguments;
        main_=MainThread{*stack,arg(0),arg(4)};
        threads_.initialize(*stack,arg(0),arg(4));
        return returned(state,stack->pointer);
    }
private:
    Stop replace_image(runtime::State& state,Memory& memory) {
        if (handlers_.active() || (memory.interrupts && memory.interrupts->active()) ||
            !threads_.can_replace_image()) return Stop::unsupported_system;
        const auto entry=runtime::default_ram_address(static_cast<std::uint32_t>(state.gpr[4].lo));
        if (!entry || entry%4 || entry>=0x2000000U) return Stop::invalid_state;
        if (!compiled_ || !compiled_(entry)) { memory.fault_address=entry; return Stop::unmapped_code; }
        LaunchArguments next;
        const auto captured=next.capture(memory,static_cast<std::uint32_t>(state.gpr[6].lo),static_cast<std::uint32_t>(state.gpr[7].lo));
        if (captured!=Stop::budget) return captured;
        if (publish_image) {
            const auto published=publish_image(memory,entry);
            if (published!=Stop::budget) return published;
        }
        // Commit after validation; a failed handoff preserves all old ownership.
        launch_=std::move(next); threads_.replace_image(); semaphores_={}; main_.reset(); heap_.reset();
        if (memory.interrupts) { memory.interrupts->replace_image(); memory.interrupts->publish(memory); }
        state.pc=entry; state.delay_slot=false; state.pending_target=0; state.branch_pc=0;
        state.set(28,static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[5].lo)))));
        state.set(4,0); // No optional host startup structure; arguments arrive through SetupThread.
        ++state.executed; return Stop::budget;
    }
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
            fields[3]=threads_.semaphore_waiters(id); fields[4]=record->attributes; fields[5]=record->option;
            std::memcpy(memory.data+address,fields.data(),24);
        } else if (number==0x41) {
            const bool released=threads_.release_semaphore_waiters(id)!=0;
            semaphores_.erase(id);
            if (released) {
                returned(state,id);
                if (!handlers_.active() && !(memory.interrupts && memory.interrupts->active())) threads_.schedule(state);
                return Stop::budget;
            }
        } else if (number==0x42 || number==0x43) {
            if (threads_.signal_semaphore(id)) {
                returned(state,id);
                if (number==0x42 && !handlers_.active() && !(memory.interrupts && memory.interrupts->active())) threads_.schedule(state);
                return Stop::budget;
            }
            if (!semaphores_.signal(id)) return Stop::unsupported_system;
        } else {
            if (!record->count) {
                if (number==0x44) {
                    if (handlers_.active() || (memory.interrupts && memory.interrupts->active())) return Stop::unsupported_system;
                    threads_.allow_idle=static_cast<bool>(idle_pump);
                    const auto blocked=threads_.wait_semaphore(id);
                    if (blocked!=Stop::budget) return blocked;
                    returned(state,id); threads_.schedule(state);
                    return Stop::budget;
                }
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
    LaunchArguments launch_;
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
    std::optional<std::chrono::steady_clock::time_point> idle_since,idle_mark;
    while (budget) {
        if (services.stop_requested && services.stop_requested()) return Stop::host_stop;
        if (services.poll_native_jobs) {
            const auto completed=services.poll_native_jobs();
            if (completed!=Stop::budget) return completed;
        }
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
            if (!idle_mark) idle_mark=idle_since;
            if (services.idle_pump) {
                const auto pumped=services.idle_pump();
                if (pumped!=Stop::budget) return pumped;
            }
            if (services.program_waiting) services.program_waiting();
            if (memory.io.poll) memory.io.poll(memory.io.context);
            if (memory.completions) memory.completions->publish(memory);
            if (memory.interrupts) memory.interrupts->publish(memory);
            if ((state.cop0_status&0x10007U)==0x10001U && (state.cop0_status&memory.pending_interrupts&0xff00U)) {
                if (services.profile) {
                    std::uint64_t key=0;
                    if (memory.interrupts) key=memory.interrupts->read(0x1000f000).value&memory.interrupts->read(0x1000f010).value&0xffffU;
                    if (memory.completions) {
                        const auto dmac=memory.completions->status();
                        key|=std::uint64_t(dmac&(dmac>>16)&0x3ffU)<<16;
                    }
                    const auto now=std::chrono::steady_clock::now();
                    auto& wake=services.profile->idle_wakes[key];
                    ++wake.calls; wake.nanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-*idle_mark).count();
                    idle_mark=now;
                }
                const auto delivered=services.interrupt(state,memory);
                if (delivered!=Stop::budget) return delivered;
                continue;
            }
            if (std::chrono::steady_clock::now()-*idle_since>=services.idle_timeout) return Stop::thread_wait;
            const auto idle_start=services.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto deadline=memory.graphics ? memory.graphics->event_wait_ns() : std::nullopt;
            native_service_wait(deadline.value_or(1000000));
            if (services.profile) services.profile->idle_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-idle_start).count();
            continue;
        }
        // An interrupt that does not wake a native continuation is not
        // foreground progress and must not restart the bounded idle timeout.
        if (!services.foreground_waiting()) idle_since.reset();
        idle_mark.reset();
        const auto before=state.executed;
        const auto entry_pc=state.pc;
        const auto slice=memory.io.poll && budget>4096 ? 4096 : budget;
        const auto entry_start=services.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto result=entry(&state,&memory,slice);
        const auto retired=state.executed-before;
        if (services.profile) {
            const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-entry_start).count());
            services.profile->entry_ns+=elapsed;
            services.profile->longest_entry_ns=std::max(services.profile->longest_entry_ns,elapsed);
            ++services.profile->entries; services.profile->operations+=retired;
            auto& sites=services.profile->sites;
            if (sites.size()<4096 || sites.contains(entry_pc)) {
                auto& site=sites[entry_pc];
                site.nanoseconds+=elapsed; site.operations+=retired; ++site.calls;
            }
        }
        if (retired>slice) return Stop::invalid_state;
        budget-=retired;
        const bool job_wait=result==Stop::native_wait || (result==Stop::vector_pending && services.native_jobs_pending && services.native_jobs_pending());
        if ((result==Stop::display_wait || job_wait) && budget) {
            if ((!job_wait && !memory.graphics) || (job_wait && (!services.poll_native_jobs || !services.native_jobs_pending))) return Stop::invalid_state;
            const auto waiting_since=std::chrono::steady_clock::now();
            const auto completed_before=services.native_jobs_completed ? services.native_jobs_completed() : 0U;
            if (services.profile) {
                if (job_wait) ++services.profile->job_waits; else ++services.profile->display_waits;
            }
            ExecutionProfile::Site* wait_site=nullptr;
            if (services.profile && job_wait && (services.profile->job_wait_sites.size()<256 ||
                    services.profile->job_wait_sites.contains((std::uint64_t(state.pc)<<8)|std::uint32_t(result)))) {
                wait_site=&services.profile->job_wait_sites[(std::uint64_t(state.pc)<<8)|std::uint32_t(result)];
                ++wait_site->calls;
            }
            for (;;) {
                if (services.stop_requested && services.stop_requested()) return Stop::host_stop;
                if (services.poll_native_jobs) {
                    const auto completed=services.poll_native_jobs();
                    if (completed!=Stop::budget) return completed;
                }
                if (services.idle_pump) {
                    const auto pumped=services.idle_pump();
                    if (pumped!=Stop::budget) return pumped;
                }
                if (memory.sif_outbound) {
                    const auto pumped=memory.sif_outbound->pump(memory);
                    if (pumped!=Stop::budget) return pumped;
                }
                if (memory.io.poll) memory.io.poll(memory.io.context);
                if (memory.completions) memory.completions->publish(memory);
                if (memory.interrupts) memory.interrupts->publish(memory);
                if (!state.delay_slot && (state.cop0_status&0x10007U)==0x10001U && (state.cop0_status&memory.pending_interrupts&0xff00U)) break;
                // A full queue needs one free place, not an empty queue: retry once a
                // submission has completed, instead of waiting for all of them.
                if (result==Stop::vector_pending && services.native_jobs_completed && services.native_jobs_completed()!=completed_before) break;
                auto delay=job_wait ? std::optional<std::uint64_t>(services.native_jobs_pending() ? 100000U : 0U) : memory.graphics->display_wait_ns();
                if (!delay || !*delay) break;
                if (memory.graphics) {
                    const auto event=memory.graphics->event_wait_ns();
                    if (event) *delay=std::min(*delay,*event);
                }
                const auto start=std::chrono::steady_clock::now();
                if (start-waiting_since>=services.idle_timeout) return job_wait ? Stop::native_wait : Stop::display_wait;
                // Vector jobs usually finish in far less time than the shortest host
                // timer sleep, so a short wait yields and polls. Longer waits sleep.
                if (job_wait && start-waiting_since<std::chrono::microseconds(200)) {
                    std::this_thread::yield();
                    if (services.profile) services.profile->job_spin_ns+=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                    continue;
                }
                // Bound host sleep by the next display deadline and the existing
                // native service-pump cadence so input, timers and IOP work run.
                native_service_wait(*delay);
                if (services.profile) {
                    const auto slept=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                    services.profile->idle_ns+=slept;
                    if (job_wait) services.profile->job_sleep_ns+=slept; else services.profile->display_sleep_ns+=slept;
                    if (wait_site) wait_site->nanoseconds+=slept;
                }
            }
            continue;
        }
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
        const auto service_start=services.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto service=services.invoke(state,memory);
        if (services.profile) services.profile->service_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-service_start).count();
        if (service!=Stop::budget) return service;
        --budget;
    }
    return Stop::budget;
}
}
