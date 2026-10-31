#include <nt/ee_sif_command_peer.hpp>
#include <nt/ee_services.hpp>
#include <nt/ee_timers.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_performance.hpp>
#include <iostream>
#include <stdexcept>
#include <array>
#include <source_location>
#include <string>
#include <vector>
#include <cfenv>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
nt::ee::Stop run(nt::runtime::State* state,std::uint64_t budget) {
    nt::ee::Memory memory;
    return nt_ee_generated_run(state,&memory,budget);
}
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
void check(bool ok,std::source_location location=std::source_location::current()) {
    if (!ok) throw std::runtime_error("EE LLVM execution mismatch at line "+std::to_string(location.line()));
}
extern "C" bool nt_ee_generated_contains(std::uint32_t);
static std::uint64_t timer_now=0;
static std::uint64_t timer_clock() { return timer_now; }
static std::uint64_t stepping_timer_clock() { timer_now+=1000; return timer_now; }
void run_ee_batch_tests();
void run_ee_file_rpc_tests();
int main() {
    try {
        {
            // Execute original LLVM counter transfers against deterministic
            // samples; these samples are test input, never a runtime fallback.
            std::uint64_t sample=100;
            bool available=true;
            nt::ee::NativePerformanceCounters cycles([&]() -> std::optional<std::uint64_t> {
                if (!available) return std::nullopt;
                return sample;
            });
            nt::ee::Memory memory; cycles.attach(memory);
            nt::runtime::State state; state.pc=0x147c;
            state.gpr[8].lo=0x8003783eU; // all-mode cycles plus unsupported cache event
            state.gpr[9].lo=0xfffffff0U; state.gpr[10].lo=29;
            check(nt_ee_generated_run(&state,&memory,4)==nt::ee::Stop::budget);
            sample+=32;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[12].lo==16);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                  state.pc==0x1490 && state.executed==5 &&
                  cycles.error()==nt::ee::NativePerformanceCounters::Error::unavailable_event);
            // Failed acquisition must not consume a transfer or mutate values.
            state.pc=0x148c; available=false;
            const auto before=memory.performance.counters[0];
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                  memory.performance.counters[0]==before && state.executed==5);
            available=true; sample=1;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                  cycles.error()==nt::ee::NativePerformanceCounters::Error::regressed_sample);
            sample=140;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[12].lo==24);
            // Disable latches the real count, but does not erase an unmeasured
            // cache interval. Explicit disabled writes recover counter state.
            state.pc=0x147c; state.gpr[8].lo=0;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
            state.pc=0x1490;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system);
            state.pc=0x1498;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  memory.performance.control==0 && memory.performance.counters[0]==0);
            state.pc=0x148c; sample+=10000;
            check(nt_ee_generated_run(&state,&memory,2)==nt::ee::Stop::budget &&
                  state.gpr[12].lo==0 && state.gpr[13].lo==0);
            state.pc=0x147c; state.gpr[8].lo=0x80000024U;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_system &&
                  state.pc==0x148c); // no mode attribution from the read's mode
            nt::ee::Memory existing;
            existing.performance.control=0x8000003eU;
            nt::ee::NativePerformanceCounters late([&] { return std::optional<std::uint64_t>(sample); });
            late.attach(existing); state={}; state.pc=0x148c;
            check(nt_ee_generated_run(&state,&existing,1)==nt::ee::Stop::unsupported_system &&
                  !state.executed); // attaching cannot recover unmeasured history
            nt::ee::NativePerformanceCounters::Values counts{100,10};
            nt::ee::NativePerformanceCounters pmu([&] {
                return std::optional<nt::ee::NativePerformanceCounters::Values>(counts);
            },true);
            nt::ee::Memory measured; pmu.attach(measured);
            state={}; state.pc=0x147c; state.gpr[8].lo=0x8003783eU;
            check(nt_ee_generated_run(&state,&measured,4)==nt::ee::Stop::budget);
            counts={180,17};
            check(nt_ee_generated_run(&state,&measured,8)==nt::ee::Stop::syscall &&
                  state.gpr[12].lo==80 && state.gpr[13].lo==7);
            counts={190,16}; state.pc=0x148c;
            check(nt_ee_generated_run(&state,&measured,1)==nt::ee::Stop::unsupported_system &&
                  pmu.error()==nt::ee::NativePerformanceCounters::Error::regressed_sample &&
                  measured.performance.counters[0]==80);
            // Native clock policy deliberately replaces both profiling events
            // with elapsed ticks; it never presents them as hardware misses.
            sample=1000; available=true;
            nt::ee::NativePerformanceCounters clock([&]() -> std::optional<std::uint64_t> {
                return available ? std::optional(sample) : std::nullopt;
            },nt::ee::NativePerformanceCounters::ClockPolicy::elapsed_time);
            nt::ee::Memory timed; clock.attach(timed);
            state={}; state.pc=0x147c; state.gpr[8].lo=0x8003783eU;
            state.gpr[9].lo=0xfffffff0U; state.gpr[10].lo=7;
            check(nt_ee_generated_run(&state,&timed,4)==nt::ee::Stop::budget);
            sample+=32;
            check(nt_ee_generated_run(&state,&timed,8)==nt::ee::Stop::syscall &&
                  state.gpr[12].lo==16 && state.gpr[13].lo==39);
            state.pc=0x148c; available=false;
            check(nt_ee_generated_run(&state,&timed,1)==nt::ee::Stop::unsupported_system &&
                  timed.performance.counters[0]==16 && timed.performance.counters[1]==39);
            available=true; sample=999;
            check(nt_ee_generated_run(&state,&timed,1)==nt::ee::Stop::unsupported_system &&
                  clock.error()==nt::ee::NativePerformanceCounters::Error::regressed_sample);
            sample=1100; state.pc=0x147c; state.gpr[8].lo=0;
            check(nt_ee_generated_run(&state,&timed,1)==nt::ee::Stop::budget);
            state.pc=0x1498;
            check(nt_ee_generated_run(&state,&timed,8)==nt::ee::Stop::syscall);
            sample+=1000; state.pc=0x148c;
            check(nt_ee_generated_run(&state,&timed,2)==nt::ee::Stop::budget &&
                  state.gpr[12].lo==0 && state.gpr[13].lo==0);
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state;
            nt::ee::NativeThreads threads;
            threads.initialize({0x8000,0x1000,0x8d60},0,0x1100);
            const std::array<std::uint32_t,9> descriptor{0,0x1200,0x9000,0x1000,0,10,0,0,0};
            std::memcpy(ram.data()+0x2000,descriptor.data(),sizeof(descriptor));
            const auto call=[&](unsigned number,unsigned a0=0,unsigned a1=0) {
                state.set(4,a0); state.set(5,a1);
                return threads.invoke(number,state,memory,[](std::uint32_t) { return true; },
                    [](std::uint32_t,std::uint32_t) { return false; });
            };
            check(call(0x32).status==nt::ee::Stop::syscall); // No runnable replacement.
            check(call(0x20,0x2000).value==2 && call(0x33,2).value==~std::uint64_t(0));
            check(call(0x22,2).status==nt::ee::Stop::budget);
            check(call(0x33,2).value==2 && call(0x34,2).value==2);
            check(call(0x35,2).value==2 && call(0x36,2).value==0);
            check(call(0x33,2).value==2);
            check(call(0x32).value==1); threads.schedule(state);
            check(call(0x2f).value==2 && call(0x32).status==nt::ee::Stop::budget);
            check(call(0x32).status==nt::ee::Stop::syscall); // Credit consumed exactly once.
            check(call(0x30,1,0x2100).value==4);
            std::uint32_t wait=0; std::memcpy(&wait,ram.data()+0x2100+36,4); check(wait==1);
            check(call(0x33,1).value==1); threads.schedule(state);
            check(call(0x2f).value==1 && call(0x33,0).value==~std::uint64_t(0));
            nt::ee::NativeThreads idle;
            idle.initialize({0x8000,0x1000,0x8d60},0,0x1100); idle.allow_idle=true;
            const auto idle_call=[&](unsigned number,unsigned id=0) {
                state.set(4,id);
                return idle.invoke(number,state,memory,nt_ee_generated_contains,nt_ee_generated_code_overlap);
            };
            const auto retired=state.executed;
            check(idle_call(0x32).status==nt::ee::Stop::budget && idle.waiting());
            check(idle_call(0x34,1).value==1); idle.schedule(state);
            check(!idle.waiting() && state.executed==retired && idle_call(0x30,1).value==1);
            // Semaphore queues are FIFO even when the later waiter has a higher
            // scheduling priority. WakeupThread grants a sleep credit only.
            threads.allow_idle=true;
            check(threads.wait_semaphore(7)==nt::ee::Stop::budget);
            threads.schedule(state);
            check(call(0x2f).value==2 && threads.wait_semaphore(7)==nt::ee::Stop::budget);
            check(threads.semaphore_waiters(7)==2 && threads.waiting());
            check(call(0x34,1).value==1 && threads.semaphore_waiters(7)==2);
            check(call(0x30,1,0x2100).value==4);
            std::array<std::uint32_t,3> waiting{};
            std::memcpy(waiting.data(),ram.data()+0x2100+36,sizeof(waiting));
            check(waiting[0]==2 && waiting[1]==7 && waiting[2]==1);
            check(!threads.signal_semaphore(8) && threads.signal_semaphore(7));
            threads.schedule(state);
            check(call(0x2f).value==1 && threads.semaphore_waiters(7)==1);
            check(call(0x32).value==1); // Preserved sleep credit, no block.
            check(threads.signal_semaphore(7) && !threads.signal_semaphore(7));
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state; state.pc=0x3a0000;
            state.set(3,0x3c); state.set(4,0); state.set(5,0x8000); state.set(6,0x1000); state.set(7,0x6000); state.set(8,0x1100);
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            unsigned pumps=0;
            services.idle_pump=[&] { ++pumps; return nt::ee::Stop::budget; };
            services.idle_timeout=std::chrono::milliseconds(0);
            state.pc=0x3a0000; state.set(3,0x32);
            const auto before=state.executed;
            check(nt::ee::run(nt_ee_generated_run,state,memory,16,services)==nt::ee::Stop::thread_wait &&
                  pumps==1 && state.executed==before+1 && state.pc==0x3a0004);
            services.idle_pump=[] { return nt::ee::Stop::unsupported_system; };
            check(nt::ee::run(nt_ee_generated_run,state,memory,16,services)==nt::ee::Stop::unsupported_system && state.executed==before+1);
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            const auto syscall=[&](unsigned number,unsigned a0=0,unsigned a1=0) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,a0); state.set(5,a1);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            state.set(6,0x1000); state.set(7,0x6000); state.set(8,0x1100);
            check(syscall(0x3c,0,0x8000)==nt::ee::Stop::budget);
            const std::array<std::uint32_t,6> descriptor{0,1,0,0,0x1234,0x5678};
            std::memcpy(ram.data()+0x2000,descriptor.data(),sizeof(descriptor));
            check(syscall(0x40,0x2000)==nt::ee::Stop::budget);
            const auto id=static_cast<unsigned>(state.gpr[2].lo);
            unsigned pumps=0;
            services.idle_pump=[&] { ++pumps; return nt::ee::Stop::budget; };
            services.idle_timeout=std::chrono::milliseconds(0);
            check(syscall(0x44,id)==nt::ee::Stop::budget);
            const auto retired=state.executed;
            check(nt::ee::run(nt_ee_generated_run,state,memory,10,services)==nt::ee::Stop::thread_wait &&
                  state.executed==retired && state.pc==0x3a0004 && pumps==1);
            nt::runtime::State signal;
            const auto native_call=[&](unsigned number,unsigned a0,unsigned a1=0) {
                signal.pc=0x3a0000; signal.set(3,number); signal.set(4,a0); signal.set(5,a1);
                return services.invoke(signal,memory);
            };
            check(native_call(0x47,id,0x2100)==nt::ee::Stop::budget);
            std::array<std::uint32_t,6> status{};
            std::memcpy(status.data(),ram.data()+0x2100,sizeof(status));
            check(status[0]==0 && status[3]==1 && status[4]==0x1234 && status[5]==0x5678);
            check(native_call(0x41,id)==nt::ee::Stop::unsupported_system);
            check(native_call(0x43,id)==nt::ee::Stop::budget && !services.waiting(memory));
            check(native_call(0x45,id)==nt::ee::Stop::budget && signal.gpr[2].lo==~std::uint64_t(0));
            check(native_call(0x47,id,0x2100)==nt::ee::Stop::budget);
            std::memcpy(status.data(),ram.data()+0x2100,sizeof(status));
            check(status[0]==0 && status[3]==0); // Direct handoff did not create another token.
        }
        for (const auto slice : {1U,2U,64U}) {
            nt::runtime::State state; state.pc=0x1000;
            state.gpr[0]={99,77};
            for (unsigned i=1;i<32;++i) state.gpr[i].hi=0x123456789abcdef0ULL;
            check(run(&state,0)==nt::ee::Stop::budget && state.executed==0);
            nt::ee::Stop result;
            do { result=run(&state,slice); } while(result==nt::ee::Stop::budget);
            check(result==nt::ee::Stop::syscall && state.pc==0x1024 && state.executed==9);
            check(state.gpr[8].lo==0xffffffff80000000ULL && state.gpr[9].lo==0x7fffffff &&
                  state.gpr[10].lo==0x80000000 && state.gpr[11].lo==0xffffffff80000001ULL &&
                  state.gpr[12].lo==0x08000000 && state.gpr[13].lo==0xfffffffff0000000ULL &&
                  state.gpr[14].lo==0 && state.gpr[15].lo==0x7ffffffe && state.gpr[0].lo==99);
            for (unsigned i=1;i<32;++i) check(state.gpr[i].hi==0x123456789abcdef0ULL);
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==9);
            state.pc=0x2000; check(run(&state,8)==nt::ee::Stop::unmapped_code);
            state.pc=0x1000; state.delay_slot=true;
            check(run(&state,8)==nt::ee::Stop::invalid_state && state.executed==9);
        }
        for (const auto slice : {1U,2U,5U,64U}) {
            nt::runtime::State state; state.pc=0x1028;
            nt::ee::Stop result=nt::ee::Stop::budget;
            for (unsigned attempt=0;attempt<64 && result==nt::ee::Stop::budget;++attempt)
                result=run(&state,slice);
            check(result==nt::ee::Stop::syscall && state.pc==0x1044 && state.executed==16 &&
                  state.gpr[8].lo==0 && state.gpr[9].lo==3 && state.gpr[10].lo==8 &&
                  state.gpr[11].lo==0x1044 && state.gpr[31].lo==0x1044 && !state.delay_slot);
        }
        {
            nt::runtime::State state; state.pc=0x105c; state.gpr[4].lo=0x1044;
            check(run(&state,1)==nt::ee::Stop::budget && state.pc==0x1060 &&
                  state.delay_slot && state.pending_target==0x1044 && state.gpr[4].lo==0x1064);
            check(run(&state,8)==nt::ee::Stop::syscall && state.pc==0x1044 &&
                  state.gpr[12].lo==1 && state.executed==2 && !state.delay_slot);
            state={}; state.pc=0x1048; state.gpr[4].lo=0x1045;
            check(run(&state,8)==nt::ee::Stop::unmapped_code && state.pc==0x1045 &&
                  state.gpr[4].lo==0 && state.executed==2 && !state.delay_slot);
            state={}; state.pc=0x1078;
            check(run(&state,8)==nt::ee::Stop::syscall && state.pc==0x1024 &&
                  state.gpr[14].lo==1 && state.executed==2);
            state={}; state.pc=0x1060; state.delay_slot=true; state.branch_pc=0x1048;
            check(run(&state,8)==nt::ee::Stop::invalid_state && !state.executed);
        }
        for (const bool equal : {false,true}) {
            nt::runtime::State state; state.pc=0x1068; state.gpr[4].lo=1;
            state.gpr[5].lo=equal ? 1 : 0x100000001ULL;
            check(run(&state,8)==nt::ee::Stop::syscall &&
                  state.pc==(equal ? 0x1074U : 0x1070U) && state.executed==2 && state.gpr[13].lo==1);
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto slice : {1U,64U}) {
            std::array<std::uint8_t,0x2000> ram{};
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::runtime::State state; state.pc=0x1080; state.gpr[4].lo=alias|0x1800;
            state.gpr[8].lo=0x87654321876580ffULL;
            for (unsigned reg=9;reg<16;++reg) state.gpr[reg].hi=123;
            nt::ee::Stop result=nt::ee::Stop::budget;
            for (unsigned attempt=0;attempt<32 && result==nt::ee::Stop::budget;++attempt)
                result=nt_ee_generated_run(&state,&memory,slice);
            check(result==nt::ee::Stop::syscall && state.pc==0x10ac && state.executed==11 &&
                  state.gpr[9].lo==0xffffffffffffffffULL && state.gpr[10].lo==0xff &&
                  state.gpr[11].lo==0xffffffffffff80ffULL && state.gpr[12].lo==0x80ff &&
                  state.gpr[13].lo==0xffffffff876580ffULL && state.gpr[14].lo==0x876580ff &&
                  state.gpr[15].lo==state.gpr[8].lo && ram[0x1808]==0xff && ram[0x1809]==0 &&
                  ram[0x180a]==0xff && ram[0x180b]==0x80 && ram[0x180c]==0xff && ram[0x180f]==0x87);
            for (unsigned reg=9;reg<16;++reg) check(state.gpr[reg].hi==123);
        }
        {
            std::array<std::uint8_t,0x2000> ram{};
            nt::ee::Memory memory{ram.data(),ram.size()};
            for (const auto address : {0x2000U,0x1801U,0x10000000U,0xa0002000U}) {
                nt::runtime::State state; state.pc=0x1080; state.gpr[4].lo=address; state.gpr[8].lo=~0ULL;
                const auto original=ram;
                check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::memory_fault &&
                      memory.fault_address==address && state.pc==0x1080 && !state.executed && ram==original);
            }
            for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
                nt::runtime::State state; state.pc=0x1080; state.gpr[4].lo=alias|0x1000;
                const auto original=ram;
                check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::code_write &&
                      memory.fault_address==(alias|0x1000) && !state.executed && ram==original);
            }
            nt::runtime::State state; state.pc=0x10b0; state.gpr[4].lo=0x2000; state.gpr[8].lo=123;
            check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::memory_fault &&
                  state.pc==0x10b4 && state.branch_pc==0x10b0 && state.pending_target==0x10bc && state.delay_slot && state.executed==1);
            state.gpr[4].lo=0x1800;
            check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::syscall && state.pc==0x10bc &&
                  !state.delay_slot && state.executed==2 && ram[0x1800]==123);
            state={}; state.pc=0x10c0; state.gpr[4].lo=0x2000;
            check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::memory_fault && !state.executed);
            state={}; state.pc=0x109c; state.gpr[4].lo=0x1ff8; memory.size=0x1fff;
            state.gpr[15].lo=456;
            check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::memory_fault && state.gpr[15].lo==456 && !state.executed);
            state.pc=0x1080; memory.data=nullptr; memory.size=0x2000;
            check(nt_ee_generated_run(&state,&memory,64)==nt::ee::Stop::memory_fault && !state.executed);
        }
        for (const auto destination : {10U,8U,9U,0U}) {
            nt::runtime::State state;
            state.pc=destination==10 ? 0x10c8 : destination==8 ? 0x10d0 : destination==9 ? 0x10d8 : 0x10e0;
            state.gpr[0]={99,77};
            state.gpr[8]={0xfffffffe00000000ULL,0x80000000ffffffffULL};
            state.gpr[9]={0x0000000100000001ULL,0x8000000000000001ULL};
            const auto before=state;
            check(run(&state,1)==nt::ee::Stop::budget && state.executed==1);
            check(run(&state,1)==nt::ee::Stop::syscall && state.executed==1);
            if (destination) check(state.gpr[destination].lo==0xffffffff00000001ULL && state.gpr[destination].hi==0xffffffffffffffffULL);
            else check(state.gpr[0].lo==99 && state.gpr[0].hi==77);
            for (unsigned reg=1;reg<32;++reg) if (reg!=destination)
                check(state.gpr[reg].lo==before.gpr[reg].lo && state.gpr[reg].hi==before.gpr[reg].hi);
        }
        {
            nt::runtime::State state; state.pc=0x10e8; state.gpr[0]={99,77}; state.gpr[9]={123,456};
            check(run(&state,8)==nt::ee::Stop::syscall && state.gpr[10].lo==123 && state.gpr[10].hi==456);
        }
        for (const bool secondary : {false,true}) {
            nt::runtime::State state; state.pc=secondary ? 0x1118 : 0x10f0;
            auto& hi=secondary ? state.hi1 : state.hi;
            auto& lo=secondary ? state.lo1 : state.lo;
            auto& other_hi=secondary ? state.hi : state.hi1;
            auto& other_lo=secondary ? state.lo : state.lo1;
            other_hi=19; other_lo=23;
            state.gpr[8].lo=0xfedcba9876543210ULL; state.gpr[9].lo=0x123456789abcdef0ULL;
            state.gpr[10].hi=123; state.gpr[11].hi=456;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 &&
                  hi==state.gpr[8].lo && lo==state.gpr[9].lo && other_hi==19 && other_lo==23 &&
                  state.gpr[10].lo==hi && state.gpr[11].lo==lo &&
                  state.gpr[10].hi==123 && state.gpr[11].hi==456);
            state.pc=secondary ? 0x112c : 0x1104; state.gpr[0]={99,77};
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==8 && hi==0 && lo==0 && other_hi==19 && other_lo==23 &&
                  state.gpr[0].lo==99 && state.gpr[0].hi==77);
        }
        for (const bool halfword : {false,true}) for (unsigned low=0;low<32;++low) {
            nt::runtime::State state; state.pc=halfword ? 0x114c : 0x1140;
            state.gpr[8].lo=0xfedcba9876543200ULL|low; state.gpr[10].hi=456;
            const auto expected=halfword ? ((low&7)^2U)*2 : (low&15)^10U;
            check(run(&state,1)==nt::ee::Stop::budget && state.sa==expected && state.executed==1);
            check(run(&state,8)==nt::ee::Stop::syscall && state.gpr[10].lo==expected && state.gpr[10].hi==456);
        }
        {
            nt::runtime::State state; state.pc=0x1158; state.sa=15; state.gpr[0]={99,77};
            check(run(&state,8)==nt::ee::Stop::syscall && state.sa==0 && state.gpr[0].lo==99 && state.gpr[0].hi==77);
        }
        for (const auto bits : {0x80000000U,0x7fc12345U,0xffffffffU,0x12345678U}) {
            nt::runtime::State state; state.pc=0x1164; state.gpr[0]={99,77};
            state.gpr[8].lo=0xfedcba9800000000ULL|bits; state.gpr[9].hi=123;
            state.fpr[3]=0xffffffff; state.fcr31=0x12345678;
            const auto expected=bits&0x80000000U ? 0xffffffff00000000ULL|bits : std::uint64_t(bits);
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 && state.fpr[2]==bits &&
                  state.fpr[3]==0 && state.gpr[9].lo==expected && state.gpr[9].hi==123 &&
                  state.gpr[0].lo==99 && state.gpr[0].hi==77 && state.fcr31==0x12345678);
        }
        struct FloatCase { std::uint32_t left,right,bits; bool underflow; };
        const FloatCase floats[]{
            {0,0,0,false},{0x80000000,0x80000000,0x80000000,false},{0x80000000,0,0,false},
            {0x3f800000,0x40000000,0x40400000,false},{0xbf800000,0x3f800000,0,false},
            {0x3f800000,0x33800000,0x3f800000,false},{0xbf800000,0xb3800000,0xbf800000,false},
            {0x00800001,0x80800000,0,true},{0x80800001,0x00800000,0x80000000,true},
            {0x00000001,0x80000001,0,false},{0x3f800000,0x80000001,0x3f800000,false},
            {0x3f800000,0x80800000,0x3f7fffff,false}
        };
        const auto rounding=std::fegetround();
        for (const auto mode : {FE_DOWNWARD,FE_UPWARD,FE_TONEAREST,FE_TOWARDZERO}) {
            check(std::fesetround(mode)==0);
            for (const auto operation : {0U,1U,2U}) for (const auto& test:floats) {
                nt::runtime::State state; state.pc=0x1178+operation*8;
                state.fpr[2]=test.left; state.fpr[3]=test.right^(operation==2 ? 0x80000000U : 0U);
                state.fpr[4]=123; state.accumulator=456; state.fcr31=0x0083c010;
                const auto before=state.fpr;
                check(run(&state,1)==nt::ee::Stop::budget && state.executed==1);
                check(run(&state,1)==nt::ee::Stop::syscall && state.executed==1 &&
                      (operation==0 ? state.accumulator : state.fpr[4])==test.bits &&
                      state.fcr31==((0x0083c010U&~0xc000U)|(test.underflow ? 0x4008U : 0U)));
                if (!operation) check(state.fpr==before);
                else check(state.accumulator==456 && state.fpr[2]==before[2] && state.fpr[3]==before[3]);
            }
        }
        check(std::fesetround(rounding)==0);
        for (const auto operands : {std::array{0x7f800000U,0U},std::array{0U,0xffc12345U},std::array{0x7f7fffffU,0x7f7fffffU}}) {
            nt::runtime::State state; state.pc=0x1190;
            state.fpr[2]=operands[0]; state.fpr[3]=operands[1]; state.accumulator=123; state.fcr31=456;
            check(run(&state,8)==nt::ee::Stop::unsupported_numeric && state.executed==1 && state.pc==0x1194 &&
                  state.delay_slot && state.pending_target==0x119c && state.accumulator==123 && state.fcr31==456);
            state.fpr[2]=0; state.fpr[3]=0;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==2 && state.pc==0x119c && !state.delay_slot);
        }
        {
            nt::runtime::State state; state.pc=0x11a0; state.fpr[2]=0x3f800000; state.fpr[3]=0x40000000;
            check(run(&state,8)==nt::ee::Stop::syscall && state.fpr[2]==0x40400000 && state.fpr[3]==0x40000000);
        }
        {
            nt::runtime::State state; state.pc=0x11a8; state.gpr[8]={123,456}; state.fpr[2]=0x12345678;
            state.accumulator=23; state.fcr31=19;
            check(run(&state,1)==nt::ee::Stop::budget && state.pc==0x11ac && state.executed==1);
            check(run(&state,8)==nt::ee::Stop::syscall && state.pc==0x11b0 && state.executed==2 &&
                  state.gpr[8].lo==123 && state.gpr[8].hi==456 && state.fpr[2]==0x12345678 &&
                  state.accumulator==23 && state.fcr31==19);
        }
        for (const auto initial : {0U,0x01000001U,0xffffffffU}) for (const auto written : {0U,0x0083c078U,0xffffffffU}) {
            nt::runtime::State state; state.pc=0x11b4; state.fcr31=initial;
            state.gpr[8].lo=0xfedcba9800000000ULL|written; state.gpr[9].hi=123; state.fpr[2]=456;
            const auto expected=(initial&~0x0083c078U)|(written&0x0083c078U);
            const auto extended=(expected&0x80000000U) ? 0xffffffff00000000ULL|expected : std::uint64_t(expected);
            check(run(&state,1)==nt::ee::Stop::budget && state.fcr31==expected);
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==2 && state.gpr[9].lo==extended &&
                  state.gpr[9].hi==123 && state.fpr[2]==456);
            state.pc=0x11c0; state.gpr[0]={99,77};
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 && state.fcr31==(initial&~0x0083c078U) &&
                  state.gpr[0].lo==99 && state.gpr[0].hi==77);
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (unsigned low=0;low<16;++low) {
            std::array<std::uint8_t,0x2000> ram{}; std::fill(ram.begin(),ram.end(),0xcc);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::runtime::State state; state.pc=0x11cc; state.gpr[4].lo=alias|0x1800|low;
            state.gpr[0]={99,77}; state.gpr[8]={0x0123456789abcdefULL,0xfedcba9876543210ULL};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==4 &&
                  state.gpr[9].lo==state.gpr[8].lo && state.gpr[9].hi==state.gpr[8].hi &&
                  state.gpr[0].lo==99 && state.gpr[0].hi==77);
            check(ram[0x17ff]==0xcc && ram[0x1820]==0xcc && ram[0x1800]==0xef && ram[0x180f]==0xfe);
            for (unsigned byte=0x1810;byte<0x1820;++byte) check(ram[byte]==0);
            state.pc=0x11e0;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  state.gpr[4].lo==state.gpr[8].lo && state.gpr[4].hi==state.gpr[8].hi);
        }
        {
            std::array<std::uint8_t,0x2000> ram{}; std::fill(ram.begin(),ram.end(),0xcc);
            const auto original=ram;
            nt::ee::Memory memory{ram.data(),0x1fff};
            for (const auto pc : {0x11ccU,0x11d0U,0x11d8U}) {
                nt::runtime::State state; state.pc=pc; state.gpr[4].lo=0xa0001fff;
                state.gpr[9]={123,456};
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault && !state.executed &&
                      state.pc==pc && memory.fault_address==0xa0001fff && state.gpr[9].lo==123 && state.gpr[9].hi==456 && ram==original);
            }
            memory.size=ram.size();
            nt::runtime::State state; state.pc=0x11cc; state.gpr[4].lo=0x8000100f;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write && !state.executed && ram==original);
        }
        {
            // One compiled-syscall lifecycle covers the native semaphore service.
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state;
            const auto call=[&](unsigned number,unsigned a,unsigned b=0) {
                state.pc=0x11ec; state.gpr[3].lo=number; state.gpr[4].lo=a; state.gpr[5].lo=b;
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            std::array<std::uint32_t,6> fields{99,2,1,99,0,123};
            std::memcpy(ram.data()+0x4000,fields.data(),24);
            check(call(0x40,0xa0004000)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            const auto original=ram;
            check(call(0x44,1)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            const auto retired=state.executed;
            check(call(0x44,1)==nt::ee::Stop::syscall && state.executed==retired && state.pc==0x11ec);
            check(call(0x45,1)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(call(0x42,1)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            state.cop0_status=0x10c02; // Service entered from an interrupt callback.
            check(call(0xffffffbdU,1)==nt::ee::Stop::budget && state.cop0_status==0x10c02);
            check(call(0x42,1)==nt::ee::Stop::syscall && ram==original);
            check(call(0x47,1,0x1000)==nt::ee::Stop::code_write && ram==original);
            check(call(0xffffffb8U,1,0xfffffff0)==nt::ee::Stop::memory_fault && ram==original);
            check(call(0xffffffb8U,1,0x4000)==nt::ee::Stop::budget);
            std::memcpy(fields.data(),ram.data()+0x4000,24);
            check(fields==std::array<std::uint32_t,6>{2,2,1,0,0,123});
            check(call(0xffffffbaU,1)==nt::ee::Stop::budget && state.cop0_status==0x10c02);
            check(call(0x41,1)==nt::ee::Stop::budget);
            check(call(0x45,1)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(call(0x40,0x4001)==nt::ee::Stop::memory_fault);
            fields[1]=0; std::memcpy(ram.data()+0x4000,fields.data(),24);
            check(call(0x40,0x4000)==nt::ee::Stop::invalid_state);
            fields[1]=2; std::memcpy(ram.data()+0x4000,fields.data(),24);
            for (unsigned id=1;id<=256;++id)
                check(call(0x40,0x4000)==nt::ee::Stop::budget && state.gpr[2].lo==id);
            check(call(0x40,0x4000)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(call(0x41,128)==nt::ee::Stop::budget);
            check(call(0x40,0x4000)==nt::ee::Stop::budget && state.gpr[2].lo==128);
            nt::ee::StartupServices independent(nt_ee_generated_code_overlap);
            state.pc=0x11ec; state.gpr[3].lo=0x45; state.gpr[4].lo=1;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,independent)==nt::ee::Stop::budget &&
                  state.gpr[2].lo==~std::uint64_t(0));
        }
        {
            nt::ee::NativeTimers timers(timer_clock);
            nt::ee::Memory memory; memory.io=timers.interface();
            // Run ordinary compiled LW/SW instructions through the native adapter.
            nt::runtime::State state;
            const auto access=[&](bool write,unsigned address,unsigned value=0) {
                state.pc=write ? 0x360000 : 0x360008;
                state.gpr[4].lo=address; state.gpr[8].lo=value;
                return nt_ee_generated_run(&state,&memory,1);
            };
            timer_now=0;
            check(access(false,0x10001810)==nt::ee::Stop::budget && state.gpr[8].lo==0);
            check(access(true,0xb0001800,65530)==nt::ee::Stop::budget);
            check(access(true,0x10001810,0x82)==nt::ee::Stop::budget);
            timer_now=1000000000;
            check(access(false,0x10001800)==nt::ee::Stop::budget && state.gpr[8].lo==51706);
            check(access(true,0x10001810,0)==nt::ee::Stop::budget);
            timer_now+=1000000000;
            check(access(false,0x10001800)==nt::ee::Stop::budget && state.gpr[8].lo==51706);
            const auto count=state.executed;
            check(access(true,0x10001810,0x183)==nt::ee::Stop::unsupported_system && state.executed==count);
            check(access(false,0x10001810)==nt::ee::Stop::budget && state.gpr[8].lo==0);
            check(access(true,0x10001820,0x12345678)==nt::ee::Stop::budget);
            check(access(false,0x10001820)==nt::ee::Stop::budget && state.gpr[8].lo==0x5678);
            check(access(false,0x10001830)==nt::ee::Stop::memory_fault);
            check(access(false,0x10001811)==nt::ee::Stop::memory_fault);
            check(access(false,0x10003800)==nt::ee::Stop::memory_fault);
            memory.io={};
            check(access(false,0x10001810)==nt::ee::Stop::memory_fault);
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            for (const auto number:{0x64U,0xffffff98U}) for (unsigned mode=0;mode<5;++mode) {
                state={}; state.pc=0x11ec; state.gpr[3].lo=number; state.gpr[4].lo=mode;
                state.gpr[2]={0xabcdef0123456789ULL,0x1234};
                const auto before=ram;
                const auto stop=nt::ee::run(nt_ee_generated_run,state,memory,1,services);
                check(stop==(mode<4 ? nt::ee::Stop::budget : nt::ee::Stop::unsupported_system));
                check(state.executed==(mode<4 ? 1U : 0U) && state.pc==(mode<4 ? 0x11f0U : 0x11ecU));
                check(state.gpr[2].lo==0xabcdef0123456789ULL && state.gpr[2].hi==0x1234 && ram==before);
            }
            // A flush never turns RAM into executable code or a registration target.
            state={}; state.pc=0x11ec; state.gpr[3].lo=0x64;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            state.pc=0x4000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==nt::ee::Stop::unmapped_code);
            state.pc=0x11ec; state.gpr[3].lo=0x74; state.gpr[4].lo=0x61; state.gpr[5].lo=0x4000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::unmapped_code);
        }
        {
            std::vector<std::uint8_t> ram(0x370100);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state;
            state.pc=0x370000; state.gpr[31].lo=0x360004;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unpublished_code && state.executed==0);
            const std::array<std::uint32_t,3> words{0x24020007U,0x03e00008U,0U};
            std::memcpy(ram.data()+0x370000,words.data(),4);
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unpublished_code && state.gpr[2].lo==0);
            std::memcpy(ram.data()+0x370000,words.data(),12);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[2].lo==7);
            ram[0x370008]=1;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unpublished_code && state.executed==1 && state.pc==0x370004);
            ram[0x370008]=0;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==3 && state.pc==0x360004);
            // Initialization copies use ordinary compiled stores, not a hidden loader.
            state.pc=0x360000; state.gpr[4].lo=0x370000; state.gpr[8].lo=words[0];
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
            state.pc=0x370000; memory.size=0x370008;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unpublished_code);
        }
        {
            std::vector<std::uint8_t> ram(0x10000); nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::NativeInterrupts interrupts; memory.interrupts=&interrupts;
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            const auto service=[&](unsigned number,unsigned line,unsigned a=0,unsigned b=0,unsigned c=0) {
                state={}; state.pc=0x11ec; state.gpr[3].lo=number; state.gpr[4].lo=line;
                state.gpr[5].lo=a; state.gpr[6].lo=b; state.gpr[7].lo=c; state.gpr[28].lo=0x4000;
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(service(0x10,11,0x80365000,0xffffffff,1)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(service(0x10,11,0x365000,0xffffffff,2)==nt::ee::Stop::budget && state.gpr[2].lo==2);
            check(service(0x10,11,0x365000,2,3)==nt::ee::Stop::budget && state.gpr[2].lo==3);
            check(service(0x14,11)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(service(0xffffffe6U,11)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            const auto deliver=[&](unsigned slice) {
                std::fill(ram.begin()+0x4000,ram.begin()+0x4004,0);
                state={}; state.pc=0x360004; state.cop0_status=0x10401; state.gpr[29].lo=0x8000;
                interrupts.raise(11);
                auto result=nt::ee::Stop::budget;
                for (unsigned tries=0;tries<64 && result==nt::ee::Stop::budget;++tries)
                    result=nt::ee::run(nt_ee_generated_run,state,memory,slice,services);
                check(result==nt::ee::Stop::syscall && state.pc==0x360004 && state.gpr[29].lo==0x8000);
                std::uint32_t log=0; std::memcpy(&log,ram.data()+0x4000,4); return log;
            };
            check(deliver(1)==0x132 && state.executed==21);
            check(service(0x11,11,3)==nt::ee::Stop::budget && state.gpr[2].lo==2);
            check(deliver(64)==0x12 && state.executed==14);
            check(service(0x10,11,0x365000,0,0xffffffff)==nt::ee::Stop::budget && state.gpr[2].lo==3);
            check(deliver(1)==0xffffffff && state.executed==7);
            check(service(0xffffffe5U,11)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(service(0x15,11)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            check(service(0x11,10,1)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(service(0x10,11,0x365000,999)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(service(0x10,16,0x365000)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(service(0x10,11,0x365001)==nt::ee::Stop::invalid_state && !state.executed);
            check(service(0x10,11,0x4000)==nt::ee::Stop::unmapped_code && !state.executed);
            for (unsigned id=4;id<=64;++id)
                check(service(0x10,11,0x365000,0xffffffff,0)==nt::ee::Stop::budget && state.gpr[2].lo==id);
            check(service(0x10,11,0x365000)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
        }
        {
            timer_now=0; nt::ee::NativePlatform platform(timer_clock);
            std::vector<std::uint8_t> ram(0x10000); nt::ee::Memory memory{ram.data(),ram.size()}; platform.attach(memory);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state; state.pc=0x11ec; state.gpr[3].lo=0x74;
            state.gpr[4].lo=0x12c; state.gpr[5].lo=0x350000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            const auto io=memory.io;
            check(io.write32(io.context,0x10001800,65535)==nt::ee::Stop::budget);
            check(io.write32(io.context,0x10001810,0xa82)==nt::ee::Stop::budget);
            timer_now=1000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==0);
            timer_now=2000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==(1U<<12));
            check((io.read32(io.context,0x10001810).value&0x800)!=0);
            state={}; state.pc=0x360004; state.cop0_status=0x10401; state.gpr[29].lo=0x8000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,32,services)==nt::ee::Stop::syscall && state.executed==3);
            timer_now+=1000000000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==0); // Latched flag suppresses another event.
            check(io.write32(io.context,0x10001810,0xa82)==nt::ee::Stop::budget);
            check((io.read32(io.context,0x10001810).value&0x800)==0);
            timer_now+=1000000000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==(1U<<12));
            platform.interrupts.write(0x1000f000,1U<<12);
            check(io.write32(io.context,0x10001810,0x800)==nt::ee::Stop::budget); // Stop and clear.
            check(io.write32(io.context,0x10001800,0)==nt::ee::Stop::budget);
            check(io.write32(io.context,0x10001820,2)==nt::ee::Stop::budget);
            check(io.write32(io.context,0x10001810,0x582)==nt::ee::Stop::budget);
            const auto start=timer_now;
            timer_now=start+3000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==0);
            timer_now=start+4000; io.poll(io.context);
            check(platform.interrupts.read(0x1000f000).value==(1U<<12));
            check((io.read32(io.context,0x10001810).value&0xc00)==0x400);
            const auto mode=io.read32(io.context,0x10001810).value;
            check(io.write32(io.context,0x10001810,0x1c2)==nt::ee::Stop::unsupported_system);
            check(io.read32(io.context,0x10001810).value==mode);
            timer_now=start; io.poll(io.context); // A backwards test clock cannot invent elapsed ticks.
            check(io.read32(io.context,0x10001800).value==2);
        }
        {
            // A compiled loop with no I/O must still give native producers time.
            timer_now=0; nt::ee::NativePlatform platform(stepping_timer_clock);
            std::vector<std::uint8_t> ram(0x10000); nt::ee::Memory memory{ram.data(),ram.size()}; platform.attach(memory);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state; state.pc=0x11ec; state.gpr[3].lo=0x10; state.gpr[4].lo=11;
            state.gpr[5].lo=0x365000; state.gpr[7].lo=1; state.gpr[28].lo=0x4000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            platform.interrupts.write(0x1000f010,1U<<11);
            const auto io=memory.io;
            check(io.write32(io.context,0x10001000,65535)==nt::ee::Stop::budget);
            check(io.write32(io.context,0x10001010,0xa82)==nt::ee::Stop::budget);
            state={}; state.pc=0x380000; state.cop0_status=0x10401; state.gpr[29].lo=0x8000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8192,services)==nt::ee::Stop::budget && state.executed==8192);
            std::uint32_t log=0; std::memcpy(&log,ram.data()+0x4000,4);
            check(log==1 && state.gpr[29].lo==0x8000);
        }
        {
            std::vector<std::uint8_t> ram(0x400000); nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state; state.pc=0x11ec; state.gpr[3].lo=0x3c;
            state.gpr[4].lo=0x6000; state.gpr[5].lo=0x20000; state.gpr[6].lo=0x1000;
            state.gpr[7].lo=0x4000; state.gpr[8].lo=0x390008;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            const auto call=[&](unsigned number,unsigned a=0,unsigned b=0) {
                state.pc=0x11ec; state.gpr[3].lo=number; state.gpr[4].lo=a; state.gpr[5].lo=b;
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            std::array<std::uint32_t,9> descriptor{0,0x390000,0x24000,0x1000,0x6000,10,0,0,7};
            std::memcpy(ram.data()+0x5000,descriptor.data(),36);
            check(call(0x20,0xa0005000)==nt::ee::Stop::budget && state.gpr[2].lo==2);
            check(call(0x2f)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(call(0xffffffd1U)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(call(0x30,2,0x5100)==nt::ee::Stop::budget && state.gpr[2].lo==0x10);
            std::array<std::uint32_t,12> status{}; std::memcpy(status.data(),ram.data()+0x5100,48);
            check(status[1]==0x390000 && status[2]==0x24000 && status[5]==10 && status[8]==7);
            check(call(0x22,2,0x12345678)==nt::ee::Stop::budget && state.gpr[2].lo==2 && state.pc==0x11f0);
            check(call(0x21,2)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            state.gpr[16].lo=0xabcdef; state.cop0_status=0x400; state.cop0_epc=0x1234;
            check(call(0x29,0,20)==nt::ee::Stop::budget && state.pc==0x390000);
            check(state.gpr[4].lo==0x12345678 && state.gpr[28].lo==0x6000 && state.gpr[29].lo==0x24d60);
            const auto retired=state.executed;
            check(nt::ee::run(nt_ee_generated_run,state,memory,3,services)==nt::ee::Stop::budget &&
                  state.executed==retired+3 && state.pc==0x11f0 && state.gpr[16].lo==0xabcdef &&
                  state.cop0_status==0x400 && state.cop0_epc==0x1234);
            std::uint32_t result=0; std::memcpy(&result,ram.data()+0x6000,4); check(result==0x12345678);
            check(call(0x21,2)==nt::ee::Stop::budget && state.gpr[2].lo==2);
            check(call(0x20,0x5000)==nt::ee::Stop::budget && state.gpr[2].lo==2);
            const auto before=ram;
            check(call(0x30,2,0x1000)==nt::ee::Stop::code_write && ram==before);
            check(call(0x20,0x5001)==nt::ee::Stop::memory_fault);
            descriptor[1]=0x4000; std::memcpy(ram.data()+0x5000,descriptor.data(),36);
            check(call(0x20,0x5000)==nt::ee::Stop::unmapped_code);
            descriptor[1]=0x390000; descriptor[5]=128; std::memcpy(ram.data()+0x5000,descriptor.data(),36);
            check(call(0x20,0x5000)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
        }
        for (const auto slice : {1U,64U}) {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativeInterrupts interrupts;
            memory.interrupts=&interrupts;
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state; state.pc=0x11ec; state.gpr[3].lo=0x74;
            state.gpr[4].lo=0x12c; state.gpr[5].lo=0x80350000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
            check(interrupts.read(0x1000f010).value==(1U<<12));
            state={}; state.pc=0x360004; state.gpr[29].lo=0x8000;
            state.cop0_status=0x10401; state.cop0_epc=0x1234;
            state.gpr[2]={41,42}; state.gpr[4].lo=23; state.gpr[31].lo=0x12345678;
            state.fpr[2]=123; state.hi=987; const auto before=state;
            interrupts.raise(12);
            auto stop=nt::ee::Stop::budget;
            for (unsigned n=0;n<16 && stop==nt::ee::Stop::budget;++n)
                stop=nt::ee::run(nt_ee_generated_run,state,memory,slice,services);
            check(stop==nt::ee::Stop::syscall && state.pc==before.pc && state.executed==3);
            check(state.gpr[2].lo==41 && state.gpr[2].hi==42 && state.gpr[4].lo==23 &&
                  state.gpr[29].lo==0x8000 && state.gpr[31].lo==0x12345678 && state.fpr[2]==123 && state.hi==987 &&
                  state.cop0_status==before.cop0_status && state.cop0_epc==before.cop0_epc);
            check(interrupts.read(0x1000f000).value==0 && !(memory.pending_interrupts&0x400));
            interrupts.raise(12); interrupts.write(0x1000f010,1U<<12);
            check(nt::ee::run(nt_ee_generated_run,state,memory,4,services)==nt::ee::Stop::syscall && state.executed==3);
            check(interrupts.read(0x1000f000).value==(1U<<12));
            interrupts.write(0x1000f010,1U<<12); state.gpr[29].lo=0x1100;
            check(nt::ee::run(nt_ee_generated_run,state,memory,4,services)==nt::ee::Stop::code_write);
            check(interrupts.read(0x1000f000).value==(1U<<12) && state.executed==3);
            interrupts.write(0x1000f000,1U<<12);
            interrupts.raise(2); interrupts.write(0x1000f010,1U<<2); state.gpr[29].lo=0x8000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,4,services)==nt::ee::Stop::interrupt_pending &&
                  interrupts.read(0x1000f000).value==(1U<<2));
        }
        for (const auto slice : {1U,64U}) {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains,0x6000);
            nt::runtime::State state;
            const auto install=[&](unsigned number,unsigned address) {
                state.pc=0x11ec; state.gpr[3].lo=0x74; state.gpr[4].lo=number; state.gpr[5].lo=address;
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(install(0x60,0xa0350010)==nt::ee::Stop::budget);
            check(install(0x61,0x350000)==nt::ee::Stop::budget);
            std::uint32_t published=0;
            std::memcpy(&published,ram.data()+0x6000+0x60*4,4);
            check(published==0xa0350010);
            std::memcpy(&published,ram.data()+0x6000+0x61*4,4);
            check(published==0x350000);
            check(install(0x61,0x35000c)==nt::ee::Stop::unmapped_code);
            check(install(0x61,0x350001)==nt::ee::Stop::invalid_state);
            check(install(0x12c,0x350000)==nt::ee::Stop::unsupported_system);
            state.pc=0x11ec; state.gpr[3].lo=0x60; state.gpr[4].lo=5;
            state.gpr[29].lo=0x8000; state.gpr[31].lo=0x12345678; state.cop0_status=0;
            const auto before=state.executed;
            auto result=nt::ee::Stop::budget;
            for (unsigned tries=0;tries<32 && result==nt::ee::Stop::budget;++tries) {
                // Stop on the restored continuation, before unrelated fixture code.
                if (state.pc==0x11f0) break;
                result=nt::ee::run(nt_ee_generated_run,state,memory,slice==1 ? 1 : 8,services);
            }
            check(result==nt::ee::Stop::budget && state.pc==0x11f0 && state.executed==before+8);
            check(state.gpr[2].lo==12 && state.gpr[29].lo==0x8000 &&
                  state.gpr[31].lo==0x12345678 && state.cop0_status==0);
            check(install(0x61,0)==nt::ee::Stop::budget);
            std::uint32_t cleared=1; std::memcpy(&cleared,ram.data()+0x6000+0x61*4,4);
            check(cleared==0);
            ram[0x6000]=1;
            const auto unchanged=state.executed;
            check(install(0x61,0x350000)==nt::ee::Stop::unsupported_system && state.executed==unchanged);
            ram[0x6000]=0;
            state.pc=0x11ec; state.gpr[3].lo=0x61;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::syscall);
            check(install(0x61,0x350000)==nt::ee::Stop::budget);
            state.pc=0x11ec; state.gpr[3].lo=0x61; state.gpr[29].lo=0x1010;
            const auto saved=ram; const auto count=state.executed;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::code_write &&
                  state.pc==0x11ec && state.executed==count && ram==saved);
        }
        for (const auto slice : {1U,64U}) {
            std::vector<std::uint8_t> ram(0x10000); std::fill(ram.begin(),ram.end(),0xcc);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state; state.pc=0x11e8;
            state.gpr[4].lo=0x1234; state.gpr[5].lo=0x8000; state.gpr[6].lo=0x1000;
            state.gpr[7].lo=0xa0004000; state.gpr[8].lo=0x1028;
            nt::ee::Stop result=nt::ee::Stop::budget;
            for (unsigned n=0;n<16 && result==nt::ee::Stop::budget;++n)
                result=nt::ee::run(nt_ee_generated_run,state,memory,slice,services);
            check(result==nt::ee::Stop::syscall && state.pc==0x11f8 && state.executed==4 &&
                  state.gpr[2].lo==0x8d60 && state.gpr[29].lo==0x8d60 && services.main_thread() &&
                  services.main_thread()->stack.base==0x8000 && services.main_thread()->global_pointer==0x1234);
            for (unsigned offset=0x4000;offset<0x4008;++offset) check(ram[offset]==0);
            check(ram[0x3fff]==0xcc && ram[0x4008]==0xcc);
            state.pc=0x11ec; state.gpr[3].lo=0x3c;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==nt::ee::Stop::syscall && state.executed==4);
        }
        for (const auto args : {0x1000U,0x4001U,0xfffcU}) {
            std::vector<std::uint8_t> ram(0x10000); std::fill(ram.begin(),ram.end(),0xcc); const auto original=ram;
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state; state.pc=0x11ec; state.gpr[3].lo=0x3c;
            state.gpr[5].lo=0xffffffff; state.gpr[6].lo=0x1000; state.gpr[7].lo=args;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==
                  (args==0x1000 ? nt::ee::Stop::code_write : nt::ee::Stop::memory_fault));
            check(!state.executed && state.pc==0x11ec && !services.main_thread() && ram==original);
            state.gpr[7].lo=0x4000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget &&
                  state.executed==1 && state.gpr[2].lo==0xed60 && services.main_thread()->stack.base==0xe000);
        }
        for (const auto slice : {1U,64U}) {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state; state.pc=0x1200; state.gpr[3].lo=0x3d;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==nt::ee::Stop::syscall && !state.executed && !services.heap());
            state.pc=0x120c; state.gpr[3].lo=0x3e;
            check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==nt::ee::Stop::syscall && !state.executed);
            state.pc=0x11ec; state.gpr[3].lo=0x3c;
            state.gpr[5].lo=0x8000; state.gpr[6].lo=0x1000; state.gpr[7].lo=0x4000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget && state.executed==1);
            state.pc=0x11fc; state.gpr[4].lo=0x5000; state.gpr[5].lo=0x1000;
            nt::ee::Stop stop=nt::ee::Stop::budget;
            for (unsigned n=0;n<16 && stop==nt::ee::Stop::budget;++n)
                stop=nt::ee::run(nt_ee_generated_run,state,memory,slice,services);
            check(stop==nt::ee::Stop::syscall && state.pc==0x1218 && state.executed==8 &&
                  services.heap() && services.heap()->begin==0x5000 && services.heap()->end==0x6000 &&
                  state.gpr[16].lo==0x6000 && state.gpr[17].lo==0x6000);
            const auto original=ram;
            for (const auto invalid : {std::array{0xfffffff0U,32U},std::array{0x7000U,0x2000U},std::array{0x9000U,0xffffffffU}}) {
                state.pc=0x1200; state.gpr[3].lo=0x3d; state.gpr[4].lo=invalid[0]; state.gpr[5].lo=invalid[1];
                check(nt::ee::run(nt_ee_generated_run,state,memory,8,services)==nt::ee::Stop::invalid_state &&
                      state.pc==0x1200 && state.executed==8 && services.heap()->end==0x6000 && ram==original);
            }
            state.gpr[4].lo=0x5000; state.gpr[5].lo=0xffffffff;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget &&
                  state.executed==9 && services.heap()->end==0x8000 && state.gpr[2].lo==0x8000 && ram==original);
            state.pc=0x1200; state.gpr[4].lo=0x8000; state.gpr[5].lo=0;
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget &&
                  services.heap()->begin==0x8000 && services.heap()->end==0x8000);
        }
        for (const auto slice : {1U,511U,512U,513U,10000U}) {
            nt::runtime::State state; state.pc=0x100000;
            nt::ee::Stop stop=nt::ee::Stop::budget;
            for (unsigned n=0;n<5000 && stop==nt::ee::Stop::budget;++n) stop=run(&state,slice);
            check(stop==nt::ee::Stop::syscall && state.pc==0x104800 && state.executed==4097 &&
                  state.gpr[20].lo==4101 && !state.delay_slot);
        }
        {
            nt::runtime::State state; state.pc=0x100000;
            check(run(&state,512)==nt::ee::Stop::budget && state.executed==512 &&
                  state.pc==0x100800 && state.delay_slot && state.pending_target==0x101000);
            check(run(&state,1)==nt::ee::Stop::budget && state.pc==0x101000 && !state.delay_slot && state.gpr[20].lo==517);
            state={}; state.pc=0x104804; state.gpr[4].lo=0x1024;
            check(run(&state,8)==nt::ee::Stop::syscall && state.pc==0x1024 && state.executed==2 && state.gpr[21].lo==1);
            state={}; state.pc=0x104804; state.gpr[4].lo=0x104805;
            check(run(&state,8)==nt::ee::Stop::unmapped_code && state.pc==0x104805 && state.executed==2);
            state={}; state.pc=0x1800;
            check(run(&state,8)==nt::ee::Stop::unmapped_code && !state.executed);
            std::vector<std::uint8_t> ram(0x110000,0xcc); const auto original=ram;
            nt::ee::Memory memory{ram.data(),ram.size()}; state={}; state.pc=0x1080; state.gpr[4].lo=0x100000;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write && !state.executed && ram==original);
        }
        for (const auto mode : {0U,8U,16U,24U}) {
            for (const auto override_bits : {0U,2U,4U,0x20000U}) {
                nt::runtime::State state; state.pc=0x121c;
                state.cop0_status=0x80400001U|mode|override_bits;
                const auto original=state.cop0_status;
                const bool allowed=!mode || override_bits;
                check(run(&state,1)==nt::ee::Stop::budget && state.executed==1 &&
                      state.cop0_status==(original|(allowed ? 0x10000U : 0U)));
                state.pc=0x1224; state.cop0_status=original|0x10000U;
                check(run(&state,1)==nt::ee::Stop::budget && state.executed==2 &&
                      state.cop0_status==(allowed ? original : original|0x10000U));
            }
        }
        {
            nt::runtime::State state; state.pc=0x121c; state.cop0_status=0x401;
            nt::ee::Memory memory; memory.pending_interrupts=0x400;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::interrupt_pending &&
                  state.pc==0x1220 && state.executed==1 && !state.gpr[22].lo && memory.pending_interrupts==0x400);
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::interrupt_pending && state.executed==1);
            memory.pending_interrupts=0; // Explicit producer acknowledgement, never done by generated code.
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.pc==0x1228 &&
                  state.executed==3 && state.gpr[22].lo==7 && state.cop0_status==0x401);
            // A pending event survives DI while IE is masked, then EI exposes it.
            state={}; state.pc=0x1224; state.cop0_status=0x10400; memory.pending_interrupts=0x400;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget &&
                  state.cop0_status==0x400 && memory.pending_interrupts==0x400);
            state={}; state.pc=0x122c; state.cop0_status=0x401;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.delay_slot);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget &&
                  !state.delay_slot && state.pc==0x1238 && state.executed==2);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::interrupt_pending &&
                  state.pc==0x1238 && state.executed==2 && !state.gpr[22].lo && memory.pending_interrupts==0x400);
            for (const auto status : {0x10001U,0x10400U,0x10403U,0x10405U}) {
                state={}; state.pc=0x1220; state.cop0_status=status;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[22].lo==7);
            }
        }
        for (const auto left : {0ULL,1ULL,0x100000000ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
            for (const auto right : {0ULL,1ULL,0x100000000ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
                nt::runtime::State state; state.pc=0x1240;
                state.gpr[8].lo=left; state.gpr[9].lo=right;
                for (unsigned r=10;r<=13;++r) state.gpr[r].hi=77;
                check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 && state.pc==0x1250);
                check(state.gpr[10].lo==((left^0x8000000000000000ULL)<(right^0x8000000000000000ULL)) &&
                      state.gpr[11].lo==(left<right) &&
                      state.gpr[12].lo==((left^0x8000000000000000ULL)<0x7fffffffffffffffULL) &&
                      state.gpr[13].lo==(left<0xffffffffffffffffULL));
                for (unsigned r=10;r<=13;++r) check(state.gpr[r].hi==77);
            }
        }
        for (const auto condition : {0ULL,1ULL,0x100000000ULL,0xffffffffffffffffULL}) {
            nt::runtime::State state; state.pc=0x1254;
            state.gpr[0]={91,92}; state.gpr[8]={0xfedcba9876543210ULL,33}; state.gpr[9]={condition,44};
            state.gpr[10]={55,66}; state.gpr[11]={77,88};
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 && state.pc==0x1264);
            check(state.gpr[10].lo==(condition ? 55 : state.gpr[8].lo) && state.gpr[10].hi==66 &&
                  state.gpr[11].lo==(condition ? state.gpr[8].lo : 77) && state.gpr[11].hi==88 &&
                  state.gpr[9].lo==(condition ? condition : state.gpr[8].lo) && state.gpr[9].hi==44 &&
                  state.gpr[0].lo==91 && state.gpr[0].hi==92);
        }
        for (unsigned variant=0;variant<10;++variant) {
            for (const auto operand : {0ULL,1ULL,0x100000000ULL,0xffffffffffffffffULL}) {
                const bool negative=(operand>>63)!=0;
                const bool conditions[]{operand==0,operand!=0,negative || !operand,!negative && operand!=0,
                                        negative,!negative,negative || !operand,!negative && operand!=0,negative,!negative};
                const bool taken=conditions[variant],likely=variant<2 || variant>=6;
                const auto entry=0x1268+variant*20;
                std::array<std::uint8_t,4> ram{42,0,0,0}; nt::ee::Memory memory{ram.data(),ram.size()};
                nt::runtime::State state; state.pc=entry; state.gpr[8].lo=operand;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.executed==1 &&
                      state.delay_slot==(!likely || taken) && state.pc==entry+((likely && !taken) ? 8 : 4));
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                      state.pc==entry+(taken ? 16 : 8) && state.gpr[10].lo==((likely && !taken) ? 0 : 42) &&
                      state.gpr[11].lo==(taken ? 9 : 0) && state.executed==(taken ? 3 : likely ? 1 : 2));
                state={}; state.pc=entry; state.gpr[8].lo=operand; state.gpr[4].lo=0x100;
                const auto result=nt_ee_generated_run(&state,&memory,8);
                check(result==((likely && !taken) ? nt::ee::Stop::syscall : nt::ee::Stop::memory_fault));
                check(state.executed==1 && state.pc==entry+((likely && !taken) ? 8 : 4));
            }
        }
        for (unsigned variant=0;variant<12;++variant) {
            for (const auto source : {0ULL,1ULL,0x80000000ULL,0x8000000000000000ULL,0xfedcba9876543210ULL}) {
                for (const auto count : {0ULL,1ULL,31ULL,32ULL,63ULL,64ULL,0xffffffffffffffffULL}) {
                    const bool word=variant>=9;
                    const unsigned bits=word ? 32 : 64;
                    const unsigned shift=variant<6 ? (variant<3 ? 31 : 63) : unsigned(count&(bits-1));
                    const auto mask=word ? 0xffffffffULL : 0xffffffffffffffffULL;
                    const auto value=source&mask;
                    auto expected=variant%3==0 ? (value<<shift)&mask : value>>shift;
                    if (variant%3==2 && shift && (value&(1ULL<<(bits-1)))) expected|=mask^(mask>>shift);
                    if (word && (expected&0x80000000ULL)) expected|=0xffffffff00000000ULL;
                    nt::runtime::State state; state.pc=0x1330+variant*8;
                    state.gpr[8].lo=count; state.gpr[9].lo=source; state.gpr[10].hi=123;
                    check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 &&
                          state.gpr[10].lo==expected && state.gpr[10].hi==123);
                }
            }
        }
        for (const auto bits : {0U,0x80000000U,1U,0x7f800000U,0x7fc12345U,0xffffffffU}) {
            for (const auto base : {4U,0x80000004U,0xa0000004U}) {
                nt::runtime::State state; state.pc=0x1390; state.gpr[4].lo=base;
                state.fpr[0]=bits; state.fcr31=0x12345678;
                std::array<std::uint8_t,8> ram{}; nt::ee::Memory memory{ram.data(),ram.size()};
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==2 &&
                      state.pc==0x1398 && state.fpr[3]==bits && state.fcr31==0x12345678);
                for (unsigned byte=0;byte<4;++byte) check(ram[byte]==((bits>>(byte*8))&255));
                state.pc=0x139c; state.fpr[0]=~bits;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.fpr[0]==bits);
            }
        }
        for (const auto pc : {0x1390U,0x1394U,0x139cU}) {
            for (const auto base : {5U,12U,0x10000004U}) {
                nt::runtime::State state; state.pc=pc; state.gpr[4].lo=base;
                state.fpr[0]=123; state.fpr[3]=456;
                std::array<std::uint8_t,8> ram{1,2,3,4,5,6,7,8}; const auto original=ram;
                nt::ee::Memory memory{ram.data(),ram.size()};
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault &&
                      !state.executed && state.pc==pc && state.fpr[0]==123 && state.fpr[3]==456 &&
                      ram==original && memory.fault_address==base-4);
            }
        }
        {
            nt::runtime::State state; state.pc=0x1390; state.gpr[4].lo=0x80001004; state.fpr[0]=123;
            std::vector<std::uint8_t> ram(0x2000,0xcc); const auto original=ram;
            nt::ee::Memory memory{ram.data(),ram.size()};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write &&
                  !state.executed && ram==original && memory.fault_address==0x80001000);
        }
        {
            struct Comparison { unsigned a,b; bool equal,less; };
            for (const auto pair : {Comparison{0,0x80000000,true,false},Comparison{1,0x80000001,true,false},
                                   Comparison{0xbf800000,0x3f800000,false,true},Comparison{0x40000000,0x3f800000,false,false},
                                   Comparison{0xbf800001,0xbf800000,false,true},Comparison{0x00800000,0,false,false}}) {
                for (unsigned predicate=0;predicate<4;++predicate) {
                    nt::runtime::State state; state.pc=0x13a4+predicate*8; state.fpr[2]=pair.a; state.fpr[3]=pair.b;
                    state.fcr31=0xffffffff;
                    const bool result=predicate==1 ? pair.equal : predicate==2 ? pair.less : predicate==3 ? pair.equal||pair.less : false;
                    check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 &&
                          state.fcr31==(result ? 0xffffffffU : 0xff7fffffU) && state.fpr[2]==pair.a && state.fpr[3]==pair.b);
                }
            }
            for (unsigned predicate=0;predicate<4;++predicate) {
                nt::runtime::State state; state.pc=0x13a4+predicate*8; state.fpr[2]=0x7fc12345; state.fcr31=0xffffffff;
                check(run(&state,8)==(predicate ? nt::ee::Stop::unsupported_numeric : nt::ee::Stop::syscall));
                check(state.executed==(predicate ? 0 : 1) && state.fcr31==(predicate ? 0xffffffffU : 0xff7fffffU));
            }
        }
        for (unsigned variant=0;variant<4;++variant) {
            for (const bool condition : {false,true}) {
                nt::runtime::State state; const auto entry=0x13c4+variant*20; state.pc=entry;
                state.fcr31=condition ? 0x800000 : 0;
                const bool taken=condition==bool(variant&1),likely=variant>=2;
                check(run(&state,1)==nt::ee::Stop::budget && state.executed==1 && state.delay_slot==(!likely||taken));
                check(run(&state,8)==nt::ee::Stop::syscall && state.pc==entry+(taken ? 16 : 8) &&
                      state.gpr[10].lo==((likely&&!taken) ? 0 : 7) && state.gpr[11].lo==(taken ? 9 : 0));
            }
        }
        {
            nt::ee::NativePlatform platform; nt::ee::Memory memory; platform.attach(memory);
            auto& owner=platform.vector_owner;
            check(memory.vectors==&owner.storage);
            nt::runtime::State state; state.pc=0x143c; state.gpr[8].lo=0x0c0c;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall);
            state={}; state.pc=0x1414;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.gpr[8].lo==0x0c0c);
            owner.storage.active=true;
            state={}; state.pc=0x143c; state.gpr[8].lo=0x202;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending && !state.executed);
            check(owner.read(29,false).value==1 && owner.read(29,true).status==nt::ee::VectorStatus::pending);
            owner.storage.active=false;
            check(owner.read(28,false).value==0x0c0c);
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && owner.read(28,false).value==0);
            owner.storage.mac=0x1234; owner.storage.status=0x5678;
            check(owner.read(17,false).value==0x1234 && owner.read(16,false).value==0x5678);
            check(owner.write(17,false,0)==nt::ee::VectorStatus::ready && owner.storage.mac==0x1234);
            check(owner.write(1,false,0xffff1234)==nt::ee::VectorStatus::ready && owner.read(1,false).value==0x1234);
            check(owner.write(0,false,42)==nt::ee::VectorStatus::ready && owner.read(0,false).value==0);
            check(owner.write(31,false,0)==nt::ee::VectorStatus::unsupported && owner.read(31,false).status==nt::ee::VectorStatus::unsupported);
        }
        {
            struct Owner {
                nt::ee::VectorReadResult response{nt::ee::VectorStatus::pending};
                unsigned calls=0,selector=0; bool wait=false;
            } owner;
            nt::ee::Memory memory;
            nt::runtime::State state; state.pc=0x1414; state.gpr[8]={123,456};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_vector &&
                  !state.executed && state.pc==0x1414 && state.gpr[8].lo==123);
            memory.vector_control={&owner,[](void* context,unsigned selector,bool wait) {
                auto& service=*static_cast<Owner*>(context);
                ++service.calls; service.selector=selector; service.wait=wait; return service.response;
            }};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending &&
                  !state.executed && owner.calls==1 && owner.selector==28 && !owner.wait && state.gpr[8].lo==123);
            owner.response={nt::ee::VectorStatus::ready,0x80001234};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  state.executed==1 && state.gpr[8].lo==0xffffffff80001234ULL && state.gpr[8].hi==456);
            state={}; state.pc=0x141c; owner.response.value=42;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && owner.wait && state.gpr[9].lo==42);
            state={}; state.pc=0x1424; state.gpr[0]={11,22}; owner.response.status=nt::ee::VectorStatus::pending;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending && !state.executed && state.gpr[0].lo==11);
            owner.response.status=nt::ee::VectorStatus::ready;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==11);
            state={}; state.pc=0x142c; owner.response.status=nt::ee::VectorStatus::pending;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending &&
                  state.pc==0x1430 && state.executed==1 && state.delay_slot && state.pending_target==0x1438);
            owner.response.status=nt::ee::VectorStatus::ready;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  state.pc==0x1438 && state.executed==2 && !state.delay_slot && state.gpr[8].lo==42);
        }
        {
            struct Owner { nt::ee::VectorStatus status=nt::ee::VectorStatus::pending; unsigned selector=0,value=77,calls=0; bool wait=false; } owner;
            nt::ee::Memory memory; nt::runtime::State state; state.pc=0x143c; state.gpr[8]={0x1234567880000002ULL,99};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_vector && !state.executed);
            memory.vector_control.context=&owner;
            memory.vector_control.write=[](void* context,unsigned selector,bool wait,std::uint32_t value) {
                auto& service=*static_cast<Owner*>(context); ++service.calls;
                if (service.status==nt::ee::VectorStatus::ready) { service.value=value; service.selector=selector; service.wait=wait; }
                return service.status;
            };
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending &&
                  !state.executed && state.pc==0x143c && owner.value==77 && owner.calls==1);
            owner.status=nt::ee::VectorStatus::ready;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 &&
                  owner.value==0x80000002 && owner.selector==28 && !owner.wait && state.gpr[8].hi==99);
            state.pc=0x1444;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && owner.wait);
            state.pc=0x144c; state.gpr[0].lo=123;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && owner.value==0 && state.gpr[0].lo==123);
        }
        {
            std::array<std::uint8_t,64> ram{};
            for (unsigned i=0;i<16;++i) ram[i]=static_cast<std::uint8_t>(i*17);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::VectorStorage vectors;
            nt::runtime::State state; state.pc=0x1454;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_vector && !state.executed);
            memory.vectors=&vectors; vectors.active=true;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending && !state.executed);
            vectors.active=false;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==4 && state.pc==0x1464);
            for (unsigned i=0;i<16;++i) check(ram[16+i]==ram[i]);
            for (unsigned i=32;i<48;++i) check(ram[i]==(i==46 ? 128 : i==47 ? 63 : 0));
            check(vectors.values[0][0]==0x33221100 && vectors.values[0][3]==0xffeeddcc);
            for (const auto base : {1U,64U,0x10000000U}) {
                state={}; state.pc=0x1454; state.gpr[4].lo=base;
                const auto original=ram; const auto registers=vectors.values;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault &&
                      !state.executed && ram==original && vectors.values==registers);
            }
            state={}; state.pc=0x1454; state.gpr[4].lo=0x80000000;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==4);
            std::vector<std::uint8_t> protected_ram(0x2000,0xcc); const auto original=protected_ram;
            memory.data=protected_ram.data(); memory.size=protected_ram.size();
            state={}; state.pc=0x1458; state.gpr[4].lo=0xff0;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write && !state.executed && protected_ram==original);
        }
        for (const auto bits : {0U,0x80000000U,1U,0x80000001U,0x3f800000U,0xff812345U}) {
            nt::runtime::State state; state.pc=0x1468; state.fpr[2]=bits; state.fcr31=0xffffffff;
            check(run(&state,1)==nt::ee::Stop::budget && state.fpr[3]==bits && state.fcr31==0xffffffff);
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==4 && state.pc==0x1478 &&
                  state.fpr[3]==bits && state.fpr[4]==(bits&0x7fffffffU) && state.fpr[5]==(bits^0x80000000U) &&
                  state.fpr[2]==(bits^0x80000000U) && state.fcr31==0xffff3fff);
        }
        {
            nt::ee::Memory memory; nt::runtime::State state; state.pc=0x147c;
            state.gpr[8].lo=0x1234567800012340ULL; state.gpr[9].lo=0x80000000; state.gpr[10].lo=0x76543210;
            for (unsigned r=11;r<=13;++r) state.gpr[r].hi=99;
            check(nt_ee_generated_run(&state,&memory,16)==nt::ee::Stop::syscall && state.executed==6 &&
                  state.pc==0x1494 && state.gpr[11].lo==0x12340 && state.gpr[12].lo==0xffffffff80000000ULL &&
                  state.gpr[13].lo==0x76543210 && memory.performance.control==0x12340);
            for (unsigned r=11;r<=13;++r) check(state.gpr[r].hi==99);
            state.pc=0x1498; state.gpr[0].lo=42;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  memory.performance.counters[0]==0 && memory.performance.counters[1]==0 && state.gpr[0].lo==42);
            state={}; state.pc=0x147c; state.gpr[8].lo=0x80000824U;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_system &&
                  state.executed==4 && state.pc==0x148c && memory.performance.control==0x80000824U &&
                  memory.metrics.unavailable_counters==3);
            state.pc=0x14a4;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && memory.performance.control==0x80000824U);
            // A configuration read is valid while measurements are unavailable.
            state={}; state.pc=0x1488; state.gpr[11].lo=123;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget &&
                  state.gpr[11].lo==0xffffffff80000824ULL);
            // Disabling alone cannot invent the missing accumulated value.
            state={}; state.pc=0x147c;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && memory.performance.control==0);
            state={}; state.pc=0x148c; state.gpr[12].lo=123;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                  !state.executed && state.gpr[12].lo==123);
            // Resetting both disabled counters restores exact known values.
            state={}; state.pc=0x1498;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  memory.metrics.unavailable_counters==0);
            state={}; state.pc=0x148c;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  state.gpr[12].lo==0 && state.gpr[13].lo==0);
            // Explicit per-counter disable selectors preserve written values,
            // even with global and all execution-mode enables set. Exercise the
            // compiled transfers, not only the native callback in isolation.
            for (const auto primary : {0U,16U}) {
                memory={}; state={}; state.pc=0x147c;
                state.gpr[8].lo=0x8000781eU|(primary<<5)|(16U<<15);
                state.gpr[9].lo=0x80000005U; state.gpr[10].lo=321;
                check(nt_ee_generated_run(&state,&memory,16)==nt::ee::Stop::syscall &&
                      state.executed==6 && memory.metrics.unavailable_counters==0 &&
                      state.gpr[12].lo==0xffffffff80000005ULL && state.gpr[13].lo==321);
            }
            // Disabling one selector must not hide an active second counter.
            memory={}; state={}; state.pc=0x147c;
            state.gpr[8].lo=0x8000781eU|(16U<<5)|(1U<<15);
            state.gpr[9].lo=17; state.gpr[10].lo=29;
            check(nt_ee_generated_run(&state,&memory,16)==nt::ee::Stop::unsupported_system &&
                  state.executed==5 && state.pc==0x1490 && state.gpr[12].lo==17 &&
                  memory.metrics.unavailable_counters==2 && memory.metrics.rejected_counter==1 &&
                  memory.metrics.rejected_operation==static_cast<unsigned>(nt::ee::PerformanceOperation::read_counter));
            // A selector change does not erase an unmeasured earlier interval.
            state={}; state.pc=0x147c;
            state.gpr[8].lo=0x8000781eU|(16U<<5)|(16U<<15);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
            state={}; state.pc=0x1490; state.gpr[13].lo=123;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                  !state.executed && state.gpr[13].lo==123);
            // An explicit write under the disabled selector restores knowledge.
            state={}; state.pc=0x1484; state.gpr[10].lo=73;
            check(nt_ee_generated_run(&state,&memory,16)==nt::ee::Stop::syscall &&
                  state.gpr[13].lo==73 && memory.metrics.unavailable_counters==0);
            struct Metrics { bool reject=true; unsigned calls=0; std::uint32_t mode=0; } metrics;
            memory.metrics={&metrics,+[](void* raw,nt::ee::PerformanceState& next,
                nt::ee::PerformanceOperation operation,unsigned counter,std::uint32_t value,std::uint32_t mode) {
                auto& metrics=*static_cast<Metrics*>(raw);
                ++metrics.calls; metrics.mode=mode;
                next.control=0x80000000U;
                if (metrics.reject) return nt::ee::IoRead{nt::ee::Stop::unsupported_system};
                using Operation=nt::ee::PerformanceOperation;
                switch (operation) {
                case Operation::write_control: next.control=value; return nt::ee::IoRead{nt::ee::Stop::budget};
                case Operation::read_control: return nt::ee::IoRead{nt::ee::Stop::budget,next.control};
                case Operation::write_counter: next.counters[counter]=value; return nt::ee::IoRead{nt::ee::Stop::budget};
                case Operation::read_counter: return nt::ee::IoRead{nt::ee::Stop::budget,next.counters[counter]};
                }
                return nt::ee::IoRead{nt::ee::Stop::invalid_state};
            }};
            memory.performance.control=0x12340; state={}; state.pc=0x147c;
            state.gpr[8].lo=0x80000000U; state.cop0_status=0xc01;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_system &&
                  !state.executed && memory.performance.control==0x12340 && metrics.mode==0xc01);
            metrics.reject=false;
            state.gpr[9].lo=0x80000005U; state.gpr[10].lo=321;
            check(nt_ee_generated_run(&state,&memory,16)==nt::ee::Stop::syscall && state.executed==6 &&
                  state.gpr[11].lo==0xffffffff80000000ULL && state.gpr[12].lo==0xffffffff80000005ULL &&
                  state.gpr[13].lo==321 && memory.performance.counters[1]==321 && metrics.calls==7);
        }
        for (const bool secondary : {false,true}) for (unsigned variant=0;variant<4;++variant) {
            for (const auto a : {0U,1U,7U,0x80000000U,0x80000001U,0xffffffffU}) {
                for (const auto b : {0U,1U,3U,0x80000000U,0xffffffffU}) {
                    const auto signed_word=[](std::uint32_t v)->std::int64_t { return v&0x80000000U ? std::int64_t(v)-0x100000000LL : v; };
                    const auto x=signed_word(a),y=signed_word(b);
                    std::uint64_t lo,hi;
                    if (variant<2) {
                        const auto product=variant==0 ? std::uint64_t(x*y) : std::uint64_t(a)*b;
                        lo=product; hi=product>>32;
                    } else if (variant==2) { lo=b ? x/y : x<0 ? 1 : -1; hi=b ? x%y : x; }
                    else { lo=b ? a/b : 0xffffffffU; hi=b ? a%b : a; }
                    lo=std::uint64_t(signed_word(std::uint32_t(lo))); hi=std::uint64_t(signed_word(std::uint32_t(hi)));
                    nt::runtime::State state; state.pc=(secondary ? 0x200150 : 0x14ac)+variant*8;
                    state.gpr[8].lo=0x1234567800000000ULL|a; state.gpr[9].lo=0xfedcba9800000000ULL|b;
                    state.gpr[10]={77,88}; state.hi=state.hi1=111; state.lo=state.lo1=222;
                    check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && (secondary ? state.lo1 : state.lo)==lo && (secondary ? state.hi1 : state.hi)==hi &&
                          state.gpr[10].lo==(variant<2 ? lo : 77) && state.gpr[10].hi==88 && (secondary ? state.hi : state.hi1)==111 && (secondary ? state.lo : state.lo1)==222);
                }
            }
        }
        {
            nt::runtime::State state; state.pc=0x14cc;
            check(run(&state,8)==nt::ee::Stop::breakpoint && !state.executed && state.pc==0x14cc);
            check(run(&state,8)==nt::ee::Stop::breakpoint && !state.executed);
            state.pc=0x14d0;
            check(run(&state,8)==nt::ee::Stop::breakpoint && state.executed==1 && state.pc==0x14d4 &&
                  state.delay_slot && state.branch_pc==0x14d0 && state.pending_target==0x14dc);
        }
        {
            struct Product { unsigned a,b,result; bool underflow=false; };
            for (const auto example : {Product{0x3fc00000,0x40000000,0x40400000},Product{0xbfc00000,0x40000000,0xc0400000},
                                      Product{0x80000000,0x3f800000,0x80000000},Product{1,0xbf800000,0x80000000},
                                      Product{0x00800000,0x3f000000,0,true},Product{0x80800000,0x3f000000,0x80000000,true},
                                      Product{0x3f800001,0x3f800001,0x3f800002}}) {
                for (const auto mode : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
                    std::fesetround(mode);
                    for (const auto pc : {0x14e0U,0x14e8U}) {
                        nt::runtime::State state; state.pc=pc; state.fpr[2]=example.a; state.fpr[3]=example.b; state.fcr31=0xffffffff;
                        check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 &&
                              state.fpr[pc==0x14e0 ? 4 : 2]==example.result &&
                              state.fcr31==(example.underflow ? 0xffff7fffU : 0xffff3fffU));
                    }
                }
            }
            std::fesetround(FE_TONEAREST);
            for (const auto pair : {Product{0x7f800000,0x3f800000,0},Product{0x3f800000,0xffffffff,0},Product{0x7f7fffff,0x40000000,0}}) {
                nt::runtime::State state; state.pc=0x14e0; state.fpr[2]=pair.a; state.fpr[3]=pair.b; state.fpr[4]=123; state.fcr31=456;
                check(run(&state,8)==nt::ee::Stop::unsupported_numeric && !state.executed && state.pc==0x14e0 &&
                      state.fpr[4]==123 && state.fcr31==456);
            }
        }
        {
            nt::ee::VectorStorage vectors; nt::ee::Memory memory; memory.vectors=&vectors;
            vectors.values[0]={0x40000000,0x40000000,0x40000000,0x40000000};
            vectors.values[1]={0x3f800000,0x40400000,0x40000000,0x40800000};
            const std::array<std::array<std::uint32_t,4>,4> expected{{
                {0x3f800000,0xbf800000,0,0xc0000000},{0x40400000,0x40a00000,0x40800000,0x40c00000},
                {0x3f800000,0x3f800000,0x3f800000,0x3f800000},{0x40a00000,0x40a00000,0x40a00000,0x40a00000}}};
            for (unsigned variant=0;variant<4;++variant) {
                nt::runtime::State state; state.pc=0x14f0+variant*8; vectors.status=0;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && vectors.values[2]==expected[variant]);
                check(vectors.mac==(variant==0 ? 0x52U : 0U) && vectors.status==(variant==0 ? 0xc3U : 0U));
            }
            nt::runtime::State state; state.pc=0x14f0; vectors.values[1][3]=0x7f800000;
            const auto original=vectors.values; const auto flags=vectors.status;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_numeric && !state.executed &&
                  vectors.values==original && vectors.status==flags);
            vectors.active=true;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending && !state.executed && vectors.values==original);
            vectors.active=false; state.pc=0x1510;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 &&
                  vectors.values[0]==std::array<std::uint32_t,4>{0x3f800000,0x40000000,0,0x40000000} && vectors.mac==2);
            state.pc=0x1518; vectors.status=0xfff; const auto before=vectors.values;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  vectors.values==before && vectors.mac==0 && vectors.status==0xff0);
        }
        {
            const std::array<std::uint32_t,4> source{0x7fc12345,0x80000000,1,0xffffffff},original{11,22,33,44};
            nt::ee::Memory memory; nt::ee::VectorStorage vectors; memory.vectors=&vectors;
            for (unsigned rotate=0;rotate<2;++rotate) for (unsigned mask=0;mask<16;++mask) {
                vectors.values[0]=source; vectors.values[1]=original; vectors.mac=123; vectors.status=456;
                nt::runtime::State state; state.pc=0x1520+(rotate*16+mask)*8;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1);
                for (unsigned lane=0;lane<4;++lane) {
                    const auto expected=(mask&(8U>>lane)) ? source[(lane+rotate)%4] : (rotate ? source[lane] : original[lane]);
                    check(vectors.values[rotate ? 0 : 1][lane]==expected);
                }
                check(vectors.mac==123 && vectors.status==456);
            }
            nt::runtime::State state; state.pc=0x1620;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall &&
                  vectors.values[1]==std::array<std::uint32_t,4>{0,0,0,0x3f800000});
            state.pc=0x1628; const auto before=vectors.values;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && vectors.values==before);
            state={}; state.pc=0x1520; vectors.active=true;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::vector_pending && !state.executed && vectors.values==before);
            memory.vectors=nullptr;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_vector && !state.executed);
        }
        {
            std::array<std::uint8_t,24> ram{}; for (unsigned n=0;n<ram.size();++n) ram[n]=std::uint8_t(n+1);
            nt::ee::Memory memory{ram.data(),ram.size()};
            for (unsigned offset=0;offset<8;++offset) {
                for (unsigned side=0;side<2;++side) {
                    nt::runtime::State state; state.pc=0x1630+side*8; state.gpr[4].lo=0x80000000+offset;
                    state.gpr[8]={0xa8a7a6a5a4a3a2a1ULL,123};
                    std::array<std::uint8_t,8> expected{0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8};
                    if (!side) for (unsigned n=0;n<=offset;++n) expected[7-offset+n]=ram[n];
                    else for (unsigned n=offset;n<8;++n) expected[n-offset]=ram[n];
                    check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[8].hi==123);
                    for (unsigned n=0;n<8;++n) check(((state.gpr[8].lo>>(n*8))&255)==expected[n]);
                }
                nt::runtime::State state; state.pc=0x1640; state.gpr[4].lo=offset;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==2);
                for (unsigned n=0;n<8;++n) check(((state.gpr[8].lo>>(n*8))&255)==ram[offset+n]);
            }
            for (const auto pc : {0x1630U,0x1638U,0x164cU}) {
                nt::runtime::State state; state.pc=pc; state.gpr[4].lo=25; state.gpr[8]={123,456}; state.gpr[0].lo=77;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault && !state.executed &&
                      state.gpr[8].lo==123 && state.gpr[8].hi==456 && state.gpr[0].lo==77 && memory.fault_address==25);
            }
        }
        {
            const std::array<std::uint8_t,8> source{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
            for (unsigned offset=0;offset<8;++offset) {
                for (unsigned side=0;side<2;++side) {
                    std::array<std::uint8_t,24> ram; std::fill(ram.begin(),ram.end(),0xcc); auto expected=ram;
                    if (!side) for (unsigned n=0;n<=offset;++n) expected[n]=source[7-offset+n];
                    else for (unsigned n=offset;n<8;++n) expected[n]=source[n-offset];
                    nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=0x1654+side*8;
                    state.gpr[4].lo=0xa0000000+offset; state.gpr[8]={0x8877665544332211ULL,99};
                    check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && ram==expected &&
                          state.gpr[8].lo==0x8877665544332211ULL && state.gpr[8].hi==99);
                }
                std::array<std::uint8_t,24> ram; std::fill(ram.begin(),ram.end(),0xcc); auto expected=ram;
                for (unsigned n=0;n<8;++n) expected[offset+n]=source[n];
                nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=0x1664;
                state.gpr[4].lo=offset; state.gpr[8].lo=0x8877665544332211ULL;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==2 && ram==expected);
            }
            for (const auto pc : {0x1654U,0x165cU,0x1670U}) {
                std::vector<std::uint8_t> ram(0x2000,0xcc); const auto original=ram;
                nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=pc; state.gpr[4].lo=0x1003;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write && !state.executed && ram==original);
                state.gpr[4].lo=0x2003;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault && !state.executed && ram==original && memory.fault_address==0x2003);
            }
            std::array<std::uint8_t,8> ram; std::fill(ram.begin(),ram.end(),0xcc); nt::ee::Memory memory{ram.data(),ram.size()};
            nt::runtime::State state; state.pc=0x1670; state.gpr[0].lo=123; state.gpr[4].lo=5;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.gpr[0].lo==123);
            for (unsigned n=0;n<8;++n) check(ram[n]==(n<5 ? 0xcc : 0));
        }
        for (unsigned selector=0;selector<3;++selector) {
            nt::runtime::State state; state.pc=0x1678+selector*12;
            state.gpr[8].lo=0x12345678ffffffffULL; state.gpr[9].hi=77;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==2 && state.gpr[9].hi==77 &&
                  state.gpr[9].lo==(selector==0 ? 0xfffffffff0c79c1fULL : 0xffffffffffffffffULL));
            check((selector==0 ? state.cop0_status : selector==1 ? state.cop0_epc : state.cop0_error_epc)==0xffffffff);
        }
        {
            nt::ee::Memory memory; memory.pending_interrupts=0x400;
            nt::runtime::State state; state.pc=0x1678; state.gpr[8].lo=0x10401; state.gpr[9].lo=123;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::interrupt_pending &&
                  state.executed==1 && state.pc==0x167c && state.cop0_status==0x10401 && state.gpr[9].lo==123);
            memory.pending_interrupts=0;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.gpr[9].lo==(0x10401U&0xf0c79c1fU));
            state.pc=0x169c; state.gpr[0].lo=42;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.cop0_status==0 && state.gpr[0].lo==42);
        }
        {
            struct Leading { unsigned bits,count; };
            for (const auto a : {Leading{0,31},Leading{0xffffffff,31},Leading{0x80000000,0},Leading{0x7fffffff,0},Leading{1,30},Leading{0xfffffff8,28}}) {
                for (const auto b : {Leading{0,31},Leading{0xffffffff,31},Leading{0x40000000,0},Leading{0x20000000,1}}) {
                    for (unsigned destination=0;destination<3;++destination) {
                        nt::runtime::State state; state.pc=0x16a4+destination*8;
                        state.gpr[8]={std::uint64_t(a.bits)|(std::uint64_t(b.bits)<<32),77}; state.gpr[9].hi=88; state.gpr[0].lo=123;
                        check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==123);
                        if (destination<2) check(state.gpr[destination ? 8 : 9].lo==(std::uint64_t(a.count)|(std::uint64_t(b.count)<<32)));
                        check(state.gpr[8].hi==77 && state.gpr[9].hi==88);
                    }
                }
            }
        }
        for (unsigned variant=0;variant<6;++variant) {
            const bool word=variant<3,immediate=variant%3==2,subtract=variant%3==1;
            nt::runtime::State state; state.pc=0x16bc+variant*8; state.gpr[8].lo=42; state.gpr[9].lo=7; state.gpr[10].hi=99;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[10].hi==99 &&
                  state.gpr[10].lo==(immediate ? 41 : subtract ? 35 : 49));
            for (unsigned side=0;side<2;++side) {
                state={}; state.pc=0x16bc+variant*8;
                const auto minimum=word ? 0xffffffff80000000ULL : 0x8000000000000000ULL;
                const auto maximum=word ? 0x7fffffffULL : 0x7fffffffffffffffULL;
                state.gpr[8].lo=immediate || side==0 ? minimum : maximum;
                state.gpr[9].lo=subtract ? (side==0 ? 1ULL : ~0ULL) : (side==0 ? ~0ULL : 1ULL);
                state.gpr[10]={123,456};
                check(run(&state,8)==nt::ee::Stop::arithmetic_overflow && !state.executed &&
                      state.pc==0x16bc+variant*8 && state.gpr[10].lo==123 && state.gpr[10].hi==456);
            }
        }
        {
            nt::runtime::State state; state.pc=0x16ec; state.gpr[0].lo=99; state.gpr[8].lo=0x7fffffff; state.gpr[9].lo=1;
            check(run(&state,8)==nt::ee::Stop::arithmetic_overflow && !state.executed && state.gpr[0].lo==99);
            state={}; state.pc=0x16f4; state.gpr[8].lo=0x80000000; state.gpr[9].lo=1;
            check(run(&state,8)==nt::ee::Stop::arithmetic_overflow && state.executed==1 && state.pc==0x16f8 &&
                  state.delay_slot && state.branch_pc==0x16f4 && state.pending_target==0x1700);
            state.gpr[8].lo=42;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==2 && state.pc==0x1700 && state.gpr[10].lo==41);
        }
        for (unsigned variant=0;variant<3;++variant) {
            unsigned index=0;
            for (const auto destination : {10U,8U,9U,0U}) {
                nt::runtime::State state; state.pc=0x1704+(variant*4+index++)*8;
                state.gpr[8]={0x8877665544332211ULL,0xfedcba9876543210ULL};
                state.gpr[9]={0x1020304050607080ULL,0x123456789abcdef0ULL}; state.gpr[0]={77,88};
                const auto expected=variant==0 ? nt::runtime::Register{state.gpr[9].lo,state.gpr[8].lo} :
                    variant==1 ? nt::runtime::Register{state.gpr[8].hi,state.gpr[9].hi} :
                    nt::runtime::Register{0x7080708070807080ULL,0xdef0def0def0def0ULL};
                check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==77 && state.gpr[0].hi==88);
                if (destination) check(state.gpr[destination].lo==expected.lo && state.gpr[destination].hi==expected.hi);
            }
        }
        {
            nt::runtime::State state; state.pc=0x1764; state.gpr[0]={77,88}; state.gpr[10]={99,111};
            check(run(&state,8)==nt::ee::Stop::syscall && state.gpr[10].lo==0 && state.gpr[10].hi==0);
        }
        for (unsigned operation=0;operation<4;++operation) {
            nt::runtime::State state; state.pc=0x176c+operation*8;
            const nt::runtime::Register a{0xabcdef0123456789ULL,0xfedcba9876543210ULL},b{0x0f0f0f0ff0f0f0f0ULL,0x123456789abcdef0ULL};
            const auto expected=[&](std::uint64_t x,std::uint64_t y) { return operation==0 ? x&y : operation==1 ? x|y : operation==2 ? x^y : ~(x|y); };
            state.gpr[8]=a; state.gpr[9]=b;
            check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 &&
                  state.gpr[8].lo==expected(a.lo,b.lo) && state.gpr[8].hi==expected(a.hi,b.hi) && state.gpr[9].lo==b.lo && state.gpr[9].hi==b.hi);
        }
        for (unsigned variant=0;variant<3;++variant) {
            const unsigned bytes=1U<<variant;
            unsigned index=0;
            for (const auto destination : {10U,8U,9U,0U}) {
                nt::runtime::State state; state.pc=0x178c+(variant*4+index++)*8;
                state.gpr[8]={0x8070605040302010ULL,0xffeeddccbbaa9988ULL};
                state.gpr[9]={0x8070605140312010ULL,0xffeeddccbbab9988ULL}; state.gpr[0]={77,88};
                const auto a=state.gpr[8],b=state.gpr[9]; nt::runtime::Register expected{};
                for (unsigned lane=0;lane<16/bytes;++lane) {
                    const auto shift=(lane*bytes%8)*8; const auto mask=bytes==4 ? 0xffffffffULL : (1ULL<<(bytes*8))-1;
                    const auto x=(lane*bytes<8 ? a.lo : a.hi)>>shift;
                    const auto y=(lane*bytes<8 ? b.lo : b.hi)>>shift;
                    if ((x&mask)==(y&mask)) (lane*bytes<8 ? expected.lo : expected.hi)|=mask<<shift;
                }
                check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==77 && state.gpr[0].hi==88);
                if (destination) check(state.gpr[destination].lo==expected.lo && state.gpr[destination].hi==expected.hi);
            }
        }
        {
            nt::runtime::State state; state.pc=0x17ec; state.gpr[0]={77,88};
            check(run(&state,8)==nt::ee::Stop::syscall && state.gpr[10].lo==~0ULL && state.gpr[10].hi==~0ULL);
        }
        {
            constexpr unsigned expanded[]{3,4,5,6,7,11,12,13,14,15,19,20,21,22,23,31};
            for (unsigned unpack=0;unpack<2;++unpack) for (unsigned bit=0;bit<32;++bit) {
                unsigned expected=0;
                if (unpack) { if (bit<16) expected=1U<<expanded[bit]; }
                else for (unsigned n=0;n<16;++n) if (expanded[n]==bit) expected=1U<<n;
                unsigned index=0;
                for (const auto destination : {10U,9U,0U}) {
                    nt::runtime::State state; state.pc=0x200000+(unpack*3+index++)*8;
                    const auto value=1U<<bit;
                    state.gpr[9]={std::uint64_t(value)|(std::uint64_t(value)<<32),std::uint64_t(value)|(std::uint64_t(value)<<32)};
                    state.gpr[0]={77,88};
                    check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==77 && state.gpr[0].hi==88);
                    if (destination) {
                        const auto half=std::uint64_t(expected)|(std::uint64_t(expected)<<32);
                        check(state.gpr[destination].lo==half && state.gpr[destination].hi==half);
                    }
                }
            }
        }
        for (unsigned kind=0;kind<3;++kind) for (unsigned width=0;width<3;++width) {
            const unsigned bytes=1U<<width,lanes=16/bytes; unsigned index=0;
            for (const auto destination : {10U,8U,9U,0U}) {
                nt::runtime::State state; state.pc=0x200030+((kind*3+width)*4+index++)*8;
                state.gpr[8]={0x0706050403020100ULL,0x0f0e0d0c0b0a0908ULL};
                state.gpr[9]={0x8786858483828180ULL,0x8f8e8d8c8b8a8988ULL}; state.gpr[0]={77,88};
                std::array<unsigned,16> expected{};
                for (unsigned lane=0;lane<lanes;++lane) {
                    const bool left=kind==0 ? lane>=lanes/2 : lane%2!=0;
                    const auto source_lane=kind==0 ? (lane%(lanes/2))*2 : lane/2+(kind==2 ? lanes/2 : 0);
                    for (unsigned byte=0;byte<bytes;++byte) expected[lane*bytes+byte]=(left ? 0 : 128)+source_lane*bytes+byte;
                }
                check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[0].lo==77 && state.gpr[0].hi==88);
                if (destination) for (unsigned byte=0;byte<16;++byte)
                    check((((byte<8 ? state.gpr[destination].lo : state.gpr[destination].hi)>>((byte%8)*8))&255)==expected[byte]);
            }
        }
        for (unsigned operation=0;operation<4;++operation) for (unsigned offset=0;offset<4;++offset) {
            std::array<std::uint8_t,12> ram{0x81,0x92,0xa3,0xb4,0xc5,0xd6,0xe7,0xf8,9,10,11,12}; const auto original=ram;
            std::array<std::uint8_t,4> reg{0x11,0x22,0x33,0xc4}; auto expected_ram=ram; auto expected_reg=reg;
            const bool store=operation>=2,right=operation%2!=0;
            if (!right) for (unsigned n=0;n<=offset;++n) {
                if (store) expected_ram[n]=reg[3-offset+n]; else expected_reg[3-offset+n]=ram[n];
            } else for (unsigned n=offset;n<4;++n) {
                if (store) expected_ram[n]=reg[n-offset]; else expected_reg[n-offset]=ram[n];
            }
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=0x200170+operation*8;
            state.gpr[4].lo=0x80000000+offset; state.gpr[8]={0x12345678c4332211ULL,99};
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && state.gpr[8].hi==99);
            if (store) check(ram==expected_ram && state.gpr[8].lo==0x12345678c4332211ULL);
            else {
                for (unsigned n=0;n<4;++n) check(((state.gpr[8].lo>>(n*8))&255)==expected_reg[n]);
                check((state.gpr[8].lo>>32)==(right && offset ? 0x12345678U : expected_reg[3]&128 ? 0xffffffffU : 0U) && ram==original);
            }
        }
        for (unsigned offset=0;offset<4;++offset) {
            std::array<std::uint8_t,12> ram{0x81,0x92,0xa3,0xb4,0xc5,0xd6,0xe7,0xf8,9,10,11,12};
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=0x200190; state.gpr[4].lo=offset;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==2 && (state.gpr[8].lo>>32)==0xffffffffU);
            for (unsigned n=0;n<4;++n) check(((state.gpr[8].lo>>(n*8))&255)==ram[offset+n]);
            state={}; state.pc=0x20019c; state.gpr[4].lo=offset; state.gpr[8].lo=0x44332211;
            auto expected=ram; for (unsigned n=0;n<4;++n) expected[offset+n]=std::uint8_t(0x11*(n+1));
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==2 && ram==expected);
        }
        for (unsigned operation=0;operation<4;++operation) {
            std::array<std::uint8_t,4> ram{0x11,0x22,0x33,0x44}; const auto original=ram;
            nt::ee::Memory memory{ram.data(),3}; nt::runtime::State state;
            state.pc=0x2001a8+operation*8; state.gpr[0]={77,88}; state.gpr[4].lo=0xa0000002;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::memory_fault &&
                  state.executed==0 && memory.fault_address==0xa0000002 && ram==original && state.gpr[0].lo==77);
            memory.size=4;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 &&
                  state.gpr[0].lo==77 && state.gpr[0].hi==88);
            if (operation<2) check(ram==original);
            else {
                for (unsigned n=0;n<4;++n) check(ram[n]==((operation==2 ? n<=2 : n>=2) ? 0 : original[n]));
                std::vector<std::uint8_t> protected_ram(0x1010,0x55);
                memory={protected_ram.data(),protected_ram.size()};
                state.pc=0x2001a8+operation*8; state.executed=0; state.gpr[4].lo=0x80001003;
                check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::code_write &&
                      state.executed==0 && memory.fault_address==0x80001003);
                for (const auto byte:protected_ram) check(byte==0x55);
            }
        }
        {
            std::vector<std::uint8_t> ram(0x4000,0xcc);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::runtime::State state;
            nt::ee::StartupServices settings(nt_ee_generated_code_overlap,nt_ee_generated_contains,0,{0x12345678,{1,2,3,4}});
            const auto call=[&](unsigned number,unsigned pointer,unsigned size=0,unsigned offset=0) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,pointer); state.set(5,size); state.set(6,offset);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,settings);
            };
            state.set(2,0xdeadbeef);
            check(call(0x4b,0xa0002000)==nt::ee::Stop::budget && state.executed==1 && state.gpr[2].lo==0xdeadbeef);
            std::uint32_t word=0; std::memcpy(&word,ram.data()+0x2000,4); check(word==0x12345678);
            word=0xfedcba98; std::memcpy(ram.data()+0x2000,&word,4);
            check(call(0x4a,0x2000)==nt::ee::Stop::budget);
            check(call(0x4b,0x2004)==nt::ee::Stop::budget);
            std::memcpy(&word,ram.data()+0x2004,4); check(word==0xfedcba98);
            check(call(0x6f,0x2009,2,1)==nt::ee::Stop::budget && ram[0x2009]==2 && ram[0x200a]==3);
            ram[0x2009]=9; ram[0x200a]=8;
            check(call(0x6e,0x2009,2,1)==nt::ee::Stop::budget);
            check(call(0x6f,0x2010,4)==nt::ee::Stop::budget && ram[0x2010]==1 && ram[0x2011]==9 && ram[0x2012]==8 && ram[0x2013]==4);
            const auto before=state.executed; const auto saved=ram;
            check(call(0x6f,0x2010,2,3)==nt::ee::Stop::invalid_state && state.executed==before && ram==saved);
            check(call(0x4b,0x1000)==nt::ee::Stop::code_write && ram==saved);
            check(call(0x4b,0x2001)==nt::ee::Stop::memory_fault && ram==saved);
            check(call(0x6e,0x3fff,2)==nt::ee::Stop::memory_fault);
            check(call(0x6f,0x2010,4)==nt::ee::Stop::budget && ram==saved);
            check(call(0x6f,0xffffffff,0,4)==nt::ee::Stop::budget);
            nt::ee::StartupServices independent(nt_ee_generated_code_overlap);
            state.pc=0x3a0000; state.set(3,0x4b); state.set(4,0x2020);
            check(nt::ee::run(nt_ee_generated_run,state,memory,1,independent)==nt::ee::Stop::budget);
            std::memcpy(&word,ram.data()+0x2020,4); check(word==0x12010);
        }
        {
            // Contract fixture only: no claim that this recorder implements mappings.
            struct Recorder { unsigned operation=99,parameter=99,value=0,calls=0; bool fail=false; } recorder;
            nt::ee::Memory memory; nt::runtime::State state;
            const auto request=+[](void* raw,nt::ee::AddressSpaceOperation operation,std::uint32_t parameter,std::uint32_t value) {
                auto& record=*static_cast<Recorder*>(raw);
                record.operation=static_cast<unsigned>(operation); record.parameter=parameter; record.value=value; ++record.calls;
                return nt::ee::IoRead{record.fail ? nt::ee::Stop::unsupported_system : nt::ee::Stop::budget,0x80000001U};
            };
            for (unsigned index=0;const auto reg:{0U,2U,3U,5U,10U}) {
                state={}; state.pc=0x3b0000+index*8; state.set(8,0x1122334455667788ULL);
                const auto pc=state.pc;
                memory.address_space={};
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && state.pc==pc && !state.executed);
                memory.address_space={&recorder,request}; recorder.fail=false;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.pc==pc+4 && state.executed==1);
                check(recorder.operation==1 && recorder.parameter==reg && recorder.value==0x55667788);
                state.gpr[9]={0x1234,0x5678}; recorder.fail=true;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && state.pc==pc+4 && state.gpr[9].lo==0x1234 && state.executed==1);
                recorder.fail=false;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.executed==2);
                check(recorder.operation==0 && recorder.parameter==reg && state.gpr[9].lo==0xffffffff80000001ULL && state.gpr[9].hi==0x5678);
                ++index;
            }
            for (unsigned index=0;index<4;++index) {
                state={}; state.pc=0x3b0100+index*4; const auto pc=state.pc;
                memory.address_space={};
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && state.pc==pc && !state.executed);
                memory.address_space={&recorder,request};
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.executed==1 && recorder.operation==index+2);
            }
        }
        {
            nt::ee::NativePlatform platform;
            nt::ee::Memory memory; platform.attach(memory);
            nt::runtime::State state;
            const auto access=[&](bool write,std::uint32_t value=0) {
                state={}; state.pc=write ? 0x360000 : 0x360008;
                state.set(4,0xb000e010); state.set(8,value);
                return nt_ee_generated_run(&state,&memory,1);
            };
            memory.pending_interrupts=0x100;
            check(access(false)==nt::ee::Stop::budget && state.gpr[8].lo==0);
            check(platform.completions.complete(5) && platform.completions.complete(7));
            check(!platform.completions.complete(10) && !platform.completions.complete(32));
            check(access(false)==nt::ee::Stop::budget && state.gpr[8].lo==0xa0 && memory.pending_interrupts==0x100);
            check(access(true,1U<<21)==nt::ee::Stop::budget && memory.pending_interrupts==0x900);
            // Mask changes neither manufacture nor discard completion events.
            check(access(true,1U<<21)==nt::ee::Stop::budget && memory.pending_interrupts==0x100);
            check(access(true,1U<<23)==nt::ee::Stop::budget && memory.pending_interrupts==0x900);
            check(access(true,1U<<7)==nt::ee::Stop::budget && memory.pending_interrupts==0x100);
            check(access(false)==nt::ee::Stop::budget && state.gpr[8].lo==0x00800020);
            check(access(true,1U<<21)==nt::ee::Stop::budget && memory.pending_interrupts==0x900);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            state={}; state.pc=0x360004; state.cop0_status=0x10801;
            check(nt::ee::run(nt_ee_generated_run,state,memory,10,services)==nt::ee::Stop::interrupt_pending && !state.executed);
            check(platform.completions.status()==0x00a00020); // No handler: event stays pending.
            check(access(true,0x00a00020)==nt::ee::Stop::budget && platform.completions.status()==0 && memory.pending_interrupts==0x100);
            check(access(true,0x80000000)==nt::ee::Stop::budget);
            check(access(false)==nt::ee::Stop::budget && state.gpr[8].lo==0xffffffff80000000ULL && memory.pending_interrupts==0x100);
            nt::ee::NativePlatform independent; check(independent.completions.status()==0);
        }
        {
            std::array<std::uint8_t,0x5000> ram{};
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::runtime::State state;
            const auto write=[&](std::uint32_t address,std::uint32_t value) {
                state={}; state.pc=0x360000; state.set(4,address); state.set(8,value);
                return nt_ee_generated_run(&state,&memory,1);
            };
            std::array<std::uint8_t,16> packet{}; packet.fill(0x5a);
            const auto deliver=[&](std::uint32_t address,bool terminal=true,bool irq=false) {
                return platform.sif_receive.deliver(memory,nt_ee_generated_code_overlap,platform.completions,address,packet,terminal,irq);
            };
            check(write(0x1000c000,0x104)==nt::ee::Stop::budget && platform.sif_receive.accepting());
            check(platform.completions.status()==0);
            const auto saved=ram;
            check(deliver(0x1000)==nt::ee::Stop::code_write && ram==saved && platform.sif_receive.accepting());
            check(deliver(0x5000)==nt::ee::Stop::memory_fault && ram==saved);
            check(write(0x1000c010,0x2000)==nt::ee::Stop::unsupported_system);
            check(deliver(0xa0002000,false)==nt::ee::Stop::budget && platform.sif_receive.accepting() && platform.completions.status()==0);
            for (unsigned i=0;i<16;++i) check(ram[0x2000+i]==0x5a);
            check(platform.sif_receive.read(0x1000c010).value==0x2010);
            check(deliver(0x2010)==nt::ee::Stop::budget && !platform.sif_receive.accepting() && platform.completions.status()==0x20);
            check(deliver(0x2020)==nt::ee::Stop::unsupported_system);
            platform.completions.acknowledge(0x20);
            check(write(0x1000c010,0x3000)==nt::ee::Stop::budget);
            check(write(0x1000c020,1)==nt::ee::Stop::budget);
            check(write(0x1000c000,0x100)==nt::ee::Stop::budget);
            check(deliver(0x4000)==nt::ee::Stop::budget && ram[0x3000]==0x5a && ram[0x4000]==0);
            check(write(0x1000c000,0x144)==nt::ee::Stop::unsupported_system);
            check(write(0x1000c000,0x184)==nt::ee::Stop::budget);
            check(deliver(0x4000,false,true)==nt::ee::Stop::budget && !platform.sif_receive.accepting());
            check(write(0x1000c000,0x104)==nt::ee::Stop::budget);
            check(write(0x1000c000,0)==nt::ee::Stop::budget && !platform.sif_receive.accepting());
            check(deliver(0x4020)==nt::ee::Stop::unsupported_system && ram[0x4020]==0);
        }
        {
            nt::ee::Memory memory; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state; state.set(2,42);
            const auto call=[&](unsigned number,unsigned id=0,unsigned value=0) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,id); state.set(5,value);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(call(0x78)==nt::ee::Stop::budget && state.gpr[2].lo==42 && platform.sif_receive.accepting());
            check(platform.sif_link.main_flags()==0x10000 && platform.completions.status()==0);
            const auto retired=state.executed;
            check(call(0x78)==nt::ee::Stop::unsupported_system && state.executed==retired);
            check(call(0x7a,4)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            check(call(0x79,1,0x2000)==nt::ee::Stop::budget && platform.sif_link.main_address()==0x2000);
            check(call(0x79,2,0x3000)==nt::ee::Stop::unsupported_system);
            platform.sif_link.publish_peer(0x4000,0x30000);
            check(call(0x7a,2)==nt::ee::Stop::budget && state.gpr[2].lo==0x4000);
            check(call(0x79,4,0x10000)==nt::ee::Stop::budget);
            check(call(0x7a,4)==nt::ee::Stop::budget && state.gpr[2].lo==0x20000);
            check(call(0x79,0x80000002,0x80000001)==nt::ee::Stop::budget);
            check(call(0x7a,0x80000002)==nt::ee::Stop::budget && state.gpr[2].lo==0xffffffff80000001ULL);
            check(call(0x6b)==nt::ee::Stop::budget && !platform.sif_receive.accepting());
            check(call(0x78)==nt::ee::Stop::budget && platform.sif_receive.accepting());
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            const auto call=[&](unsigned number,unsigned line,unsigned arg=0,unsigned next=0,unsigned callback_arg=0) {
                state.pc=0x3a0000; state.cop0_status=0; state.set(3,number); state.set(4,line); state.set(5,arg);
                state.set(6,next); state.set(7,callback_arg); state.set(28,0x4000);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(call(0x12,5,0x365000,0,1)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(call(0x10,5,0x365000,0,2)==nt::ee::Stop::budget && state.gpr[2].lo==1); // Separate handle domains.
            check(call(0x16,5)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(call(0xffffffe4U,5)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            check(call(0x14,5)==nt::ee::Stop::budget);
            platform.sif_receive.write(0x1000c000,0x184);
            std::array<std::uint8_t,16> packet{}; packet.fill(0x69);
            check(platform.sif_receive.deliver(memory,nt_ee_generated_code_overlap,platform.completions,0x5000,packet)==nt::ee::Stop::budget);
            platform.interrupts.raise(5); platform.interrupts.publish(memory);
            state={}; state.pc=0x380000; state.cop0_status=0x10c01; state.set(29,0x1200); state.set(16,0xabcd);
            check(nt::ee::run(nt_ee_generated_run,state,memory,2,services)==nt::ee::Stop::code_write && platform.completions.ready()==0x20);
            state.set(29,0x8000);
            check(nt::ee::run(nt_ee_generated_run,state,memory,2,services)==nt::ee::Stop::budget && platform.interrupts.active());
            check(platform.completions.ready()==0 && platform.interrupts.read(0x1000f000).value==0x20);
            check(nt::ee::run(nt_ee_generated_run,state,memory,5,services)==nt::ee::Stop::budget && !platform.interrupts.active());
            std::uint32_t log=0; std::memcpy(&log,ram.data()+0x4000,4);
            check(log==1 && ram[0x5000]==0x69 && state.pc==0x380000 && state.gpr[16].lo==0xabcd && state.gpr[29].lo==0x8000 && state.executed==7);
            check(nt::ee::run(nt_ee_generated_run,state,memory,7,services)==nt::ee::Stop::budget);
            std::memcpy(&log,ram.data()+0x4000,4); check(log==0x12 && state.executed==14 && state.pc==0x380000);
            check(call(0x13,5,1)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            check(call(0x11,5,1)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            check(call(0xffffffe3U,5)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            check(call(0x16,10)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
        }
        {
            struct Peer {
                std::vector<std::uint8_t> ram=std::vector<std::uint8_t>(0x1000);
                bool ready=false,fail=false;
                unsigned accepted=0;
            } peer;
            std::vector<std::uint8_t> ram(0x8000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state;
            const auto call=[&](unsigned number,unsigned pointer,unsigned count=0) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,pointer); state.set(5,count);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            const auto sink=+[](void* context,std::span<const nt::ee::NativeSifPacket> packets) {
                auto& peer=*static_cast<Peer*>(context);
                if (peer.fail) return nt::ee::IoRead{nt::ee::Stop::unsupported_system};
                if (!peer.ready) return nt::ee::IoRead{nt::ee::Stop::budget,0};
                for (const auto& packet:packets)
                    if (std::uint64_t(packet.destination)+packet.bytes.size()>peer.ram.size()) return nt::ee::IoRead{nt::ee::Stop::memory_fault};
                for (const auto& packet:packets) std::copy(packet.bytes.begin(),packet.bytes.end(),peer.ram.begin()+packet.destination);
                ++peer.accepted; return nt::ee::IoRead{nt::ee::Stop::budget,1};
            };
            std::array<std::uint32_t,8> descriptors{0xa0004000,0x100,17,0x40,0x5000,0x200,16,4};
            std::memcpy(ram.data()+0x3000,descriptors.data(),32);
            std::fill_n(ram.data()+0x4000,32,0x35); std::fill_n(ram.data()+0x5000,16,0x76);
            check(call(0x77,0x3000,2)==nt::ee::Stop::unsupported_system && !state.executed);
            check(platform.sif_outbound.connect({&peer,sink}));
            descriptors[4]=0x7ff0; descriptors[6]=32; std::memcpy(ram.data()+0x3000,descriptors.data(),32);
            check(call(0x77,0x3000,2)==nt::ee::Stop::memory_fault && memory.fault_address==0x7ff0 && !peer.accepted);
            descriptors[4]=0x5000; descriptors[6]=16; std::memcpy(ram.data()+0x3000,descriptors.data(),32);
            check(call(0xffffff89U,0xa0003000,2)==nt::ee::Stop::budget && state.gpr[2].lo==1);
            std::fill_n(ram.data()+0x4000,32,0xff); std::fill_n(ram.data()+0x3000,32,0);
            check(!platform.sif_outbound.connect({&peer,sink}));
            check(call(0xffffff8aU,1)==nt::ee::Stop::budget && state.gpr[2].lo==1 && !peer.accepted);
            check(call(0x77,0,1)==nt::ee::Stop::budget && state.gpr[2].lo==0);
            peer.fail=true; const auto retired=state.executed;
            check(call(0x76,1)==nt::ee::Stop::unsupported_system && state.executed==retired && platform.completions.status()==0);
            peer.fail=false; peer.ready=true;
            check(call(0x76,1)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(peer.accepted==1 && platform.completions.status()==0x40);
            for (unsigned i=0;i<32;++i) check(peer.ram[0x100+i]==0x35);
            for (unsigned i=0;i<16;++i) check(peer.ram[0x200+i]==0x76);
            check(call(0x76,999)==nt::ee::Stop::budget && peer.accepted==1);
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::ee::NativeSifCommandPeer peer(platform.sif_link,memory,nt_ee_generated_code_overlap,0x1000);
            check(peer.connect(platform.sif_outbound) && !peer.connect(platform.sif_outbound));
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            nt::runtime::State state;
            const auto call=[&](unsigned number,unsigned argument,unsigned count=0) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,argument); state.set(5,count);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(call(0x7a,4)==nt::ee::Stop::budget && state.gpr[2].lo==0x30000);
            check(call(0x7a,2)==nt::ee::Stop::budget && state.gpr[2].lo==0x1000);
            const auto submit=[&](std::uint32_t command,std::uint32_t a,std::uint32_t b=0) {
                const std::array<std::uint32_t,8> packet{24,0,command,0,a,b,0,0};
                const std::array<std::uint32_t,4> descriptor{0x4000,0x1000,24,0x40};
                std::memcpy(ram.data()+0x4000,packet.data(),32); std::memcpy(ram.data()+0x3000,descriptor.data(),16);
                return call(0x77,0x3000,1);
            };
            check(submit(0x80000002,0xa0005000)==nt::ee::Stop::budget);
            check(call(0x76,1)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(peer.receive_address()==0x5000 && platform.completions.status()==0x40);
            check(submit(0x80000001,7,0x1234)==nt::ee::Stop::budget);
            check(call(0x76,2)==nt::ee::Stop::budget && peer.software_register(7)==0x1234);
            platform.completions.acknowledge(0x40);
            check(submit(0x80000003,0)==nt::ee::Stop::budget); // Reboot is not silently acknowledged.
            const auto retired=state.executed;
            check(call(0x76,3)==nt::ee::Stop::unsupported_system && state.executed==retired);
            check(platform.sif_outbound.status(3)==1 && platform.completions.status()==0 && peer.receive_address()==0x5000);
        }
        {
            std::vector<std::uint8_t> ram(0x10000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::ee::NativeSifCommandPeer peer(platform.sif_link,memory,nt_ee_generated_code_overlap,0x1000,&platform.sif_receive);
            check(peer.connect(platform.sif_outbound));
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            const auto call=[&](unsigned number,unsigned a=0,unsigned b=0,unsigned c=0,unsigned d=0) {
                state.pc=0x3a0000; state.cop0_status=0; state.set(3,number); state.set(4,a); state.set(5,b); state.set(6,c); state.set(7,d); state.set(28,0x6000);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            const auto submit=[&](const std::array<std::uint32_t,16>& words) {
                std::memcpy(ram.data()+0x4000,words.data(),64);
                const std::array<std::uint32_t,4> descriptor{0x4000,0x1000,words[0],0x40};
                std::memcpy(ram.data()+0x3000,descriptor.data(),16);
                return call(0x77,0x3000,1);
            };
            check(submit({20,0,0x80000002,0,0x5000})==nt::ee::Stop::budget);
            check(call(0x76,1)==nt::ee::Stop::budget);
            platform.completions.acknowledge(0x40);
            check(call(0x12,5,0x3c0000,0,1)==nt::ee::Stop::budget);
            check(call(0x16,5)==nt::ee::Stop::budget);
            check(submit({16,0,0x80000002,1})==nt::ee::Stop::budget);
            check(call(0x76,2)==nt::ee::Stop::budget && state.gpr[2].lo==1 && !platform.completions.ready());
            platform.sif_receive.write(0x1000c000,0x184);
            const auto full_size=memory.size; memory.size=0x5001;
            const auto retired=state.executed;
            check(call(0x76,2)==nt::ee::Stop::memory_fault && state.executed==retired && platform.sif_outbound.status(2)==1);
            memory.size=full_size;
            check(call(0x76,2)==nt::ee::Stop::budget && state.gpr[2].lo==~std::uint64_t(0));
            check(ram[0x5000]==24 && ram[0x5014]==1 && platform.completions.ready()==0x20);
            state=nt::ee::NativePlatform::initial_state(0x121c); state.set(29,0x9000);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.cop0_status==0x10c01 && platform.completions.ready()==0x20);
            state.pc=0x380000;
            check(nt::ee::run(nt_ee_generated_run,state,memory,5,services)==nt::ee::Stop::budget && !platform.interrupts.active());
            std::uint32_t log=0; std::memcpy(&log,ram.data()+0x6000,4); check(log==5 && state.pc==0x380000 && platform.sif_receive.accepting());
            check(submit({36,0,0x80000009,0,0,123,0,0x7000,0xdeadbeef})==nt::ee::Stop::budget);
            check(call(0x76,3)==nt::ee::Stop::budget);
            std::array<std::uint32_t,16> response{}; std::memcpy(response.data(),ram.data()+0x5000,64);
            check(response[0]==64 && response[2]==0x80000008 && response[5]==123 && response[7]==0x7000 && response[8]==0x80000009 && !response[9] && !response[10]);
        }
        {
            std::vector<std::uint8_t> ram(0x300000);
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::runtime::State state;
            const auto access=[&](std::uint32_t address,bool write) {
                state={}; state.pc=write ? 0x360000 : 0x360008; state.set(4,address); state.set(8,0x89abcdef);
                return nt_ee_generated_run(&state,&memory,1);
            };
            for (const auto base:{0x20000000U,0x30000000U}) {
                check(access(base+0x180000,true)==nt::ee::Stop::budget);
                check(access(0x180000,false)==nt::ee::Stop::budget && state.gpr[8].lo==0xffffffff89abcdefULL);
                check(access(base+0x180000,false)==nt::ee::Stop::budget && state.gpr[8].lo==0xffffffff89abcdefULL);
                check(access(base+0x100000,false)==nt::ee::Stop::budget); // First mapped byte.
                check(access(base+0xffffc,false)==nt::ee::Stop::memory_fault && memory.fault_address==base+0xffffc);
                check(access(base+0x2000000,false)==nt::ee::Stop::memory_fault);
                check(access(base+0x300000,false)==nt::ee::Stop::memory_fault); // Actual backing is shorter.
                check(access(base+0x200000,true)==nt::ee::Stop::code_write && !state.executed);
                check(access(base+0x180001,false)==nt::ee::Stop::memory_fault && !state.executed);
                nt::ee::StartupServices services(nt_ee_generated_code_overlap);
                state={}; state.pc=0x3a0000; state.set(3,0x4b); state.set(4,base+0x180000);
                check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget);
                std::uint32_t config=0; std::memcpy(&config,ram.data()+0x180000,4); check(config==0x12010);
                platform.sif_receive.write(0x1000c000,0x184);
                std::array<std::uint8_t,16> packet{}; packet.fill(0x7a);
                check(platform.sif_receive.deliver(memory,nt_ee_generated_code_overlap,platform.completions,base+0x180000,packet)==nt::ee::Stop::budget);
                check(ram[0x180000]==0x7a && platform.sif_receive.read(0x1000c010).value==0x180010);
            }
        }
        {
            // Byte, word, doubleword and quadword accesses share the native workspace.
            std::vector<std::uint8_t> ram(0x10000,0xcc);
            const auto original=ram;
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::NativePlatform platform; platform.attach(memory);
            nt::runtime::State state;
            state.gpr[8]={0x1234567812345678ULL,0xabcdefff12345678ULL};
            for (unsigned index=0;const auto width:{16U,8U,4U,1U}) {
                const auto pc=0x3d0000+index++*16;
                for (const auto offset:{0x1000U,nt::ee::scratchpad_size-width}) {
                    state.pc=pc; state.set(4,nt::ee::scratchpad_begin+offset);
                    check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && ram==original);
                    state.pc=pc+8; state.gpr[9]={};
                    check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
                    const auto mask=width>=8 ? ~std::uint64_t(0) : (std::uint64_t(1)<<(width*8))-1;
                    check(state.gpr[9].lo==(state.gpr[8].lo&mask));
                    if (width==16) check(state.gpr[9].hi==state.gpr[8].hi);
                }
            }
            const auto pointer=memory.scratchpad;
            for (const auto raw:{0x70001001U,0x70004000U}) {
                state.pc=0x3d0020; state.set(4,raw); const auto retired=state.executed;
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::memory_fault && state.executed==retired && memory.fault_address==raw);
            }
            state.pc=0x3d0020; state.set(4,0x70001000); memory.scratchpad=nullptr;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::memory_fault);
            memory.scratchpad=pointer; memory.scratchpad_bytes=0x1001;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::memory_fault);
            nt::ee::NativePlatform independent; independent.attach(memory);
            check(memory.scratchpad!=pointer && memory.scratchpad[0x1000]==0 && ram==original);
        }
        {
            // Compiled SD/LD use atomic native 64-bit service calls.
            nt::ee::Memory memory;
            struct Backend { std::uint64_t value=0; bool fail=false,reset_fail=false; unsigned calls=0,resets=0; } backend;
            memory.io.context=&backend;
            memory.io.write64=[](void* context,std::uint32_t,std::uint64_t value) {
                auto& b=*static_cast<Backend*>(context); ++b.calls;
                if (b.fail) return nt::ee::Stop::unsupported_system;
                b.value=value; return nt::ee::Stop::budget;
            };
            memory.io.read64=[](void* context,std::uint32_t) {
                auto& b=*static_cast<Backend*>(context); ++b.calls;
                return nt::ee::IoRead64{b.fail ? nt::ee::Stop::unsupported_system : nt::ee::Stop::budget,b.value};
            };
            nt::runtime::State state; state.pc=0x3d0010; state.set(4,0x12001000); state.set(8,0xfedcba9876543210ULL);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && backend.value==state.gpr[8].lo && backend.calls==1);
            state.pc=0x3d0018; state.gpr[9]={7,99};
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[9].lo==backend.value && state.gpr[9].hi==99);
            backend.fail=true; const auto retired=state.executed; state.pc=0x3d0018; state.set(9,7);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && state.gpr[9].lo==7 && state.executed==retired && state.pc==0x3d0018);
            const auto calls=backend.calls; state.set(4,0x12001004);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::memory_fault && backend.calls==calls);
            nt::ee::NativePlatform platform; platform.attach(memory);
            state.pc=0x3d0010; state.set(4,0x12001000); state.set(8,0x200);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && !platform.graphics.resets());
            platform.graphics.context=&backend;
            platform.graphics.synchronize=[](void* context) {
                auto& b=*static_cast<Backend*>(context); ++b.calls;
                return b.fail ? nt::ee::Stop::unsupported_system : nt::ee::Stop::budget;
            };
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system && !platform.graphics.resets());
            backend.fail=false;
            platform.graphics.reset_drawing=[](void* context) {
                auto& b=*static_cast<Backend*>(context);
                if (b.reset_fail) return nt::ee::Stop::unsupported_system;
                ++b.resets; return nt::ee::Stop::budget;
            };
            backend.reset_fail=true;
            const auto before_reset=state.executed;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::unsupported_system &&
                !platform.graphics.resets() && !backend.resets && state.pc==0x3d0010 && state.executed==before_reset);
            backend.reset_fail=false;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && platform.graphics.resets()==1 && backend.resets==1);
            state.pc=0x3d0018;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[9].lo==0x4000);
            state.pc=0x3d0010; state.set(4,0x12001010); state.set(8,0);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
            state.pc=0x3d0018;
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && state.gpr[9].lo==0x6000);
            state.pc=0x3d0010; state.set(4,0x12001000); state.set(8,0x100);
            check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget && platform.graphics.resets()==1);
            nt::ee::StartupServices services(nt_ee_generated_code_overlap);
            for (const auto number:{0x71U,0xffffff8fU}) {
                state.pc=0x3a0000; state.set(3,number); state.set(4,0x100); state.set(2,123);
                check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget && state.gpr[2].lo==123);
                state.pc=0x3a0000; state.set(3,number==0x71 ? 0x70 : 0xffffff90U);
                check(nt::ee::run(nt_ee_generated_run,state,memory,1,services)==nt::ee::Stop::budget && state.gpr[2].lo==0x6100);
            }
        }
        {
            timer_now=0;
            std::vector<std::uint8_t> ram(0x10000,0);
            nt::ee::Memory memory{ram.data(),ram.size()};
            nt::ee::NativePlatform platform(timer_clock); platform.attach(memory);
            bool fail=false; platform.graphics.context=&fail;
            std::vector<bool> edges_seen;
            platform.graphics.edge_context=&edges_seen;
            platform.graphics.display_edge=[](void* raw,bool end) { static_cast<std::vector<bool>*>(raw)->push_back(end); };
            platform.graphics.synchronize=[](void* p) { return *static_cast<bool*>(p) ? nt::ee::Stop::unsupported_system : nt::ee::Stop::budget; };
            nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
            nt::runtime::State state;
            const auto configure=[&](unsigned standard,unsigned frame=1) {
                state.pc=0x3a0000; state.cop0_status=0; state.set(3,2); state.set(4,1); state.set(5,standard); state.set(6,frame); state.set(2,123);
                return nt::ee::run(nt_ee_generated_run,state,memory,1,services);
            };
            check(configure(2)==nt::ee::Stop::budget && state.gpr[2].lo==123 && platform.graphics.read(0x12000020).value==3);
            const auto edge=[](std::uint64_t ticks) { return (ticks*2002000+62)/63; };
            const auto poll=[&] { memory.io.poll(memory.io.context); platform.interrupts.publish(memory); };
            state.set(4,2); state.set(5,0x3e0000); state.set(6,0); state.set(7,0); state.set(28,0x6000);
            check(platform.interrupts.service(0x10,state,nt_ee_generated_contains).status==nt::ee::Stop::budget);
            platform.interrupts.write(0x1000f010,13);
            timer_now=edge(480)-1; poll(); check(!memory.pending_interrupts);
            timer_now=edge(480); poll(); check(memory.pending_interrupts==0x400);
            state.pc=0x3a0000; state.cop0_status=0x10c01; state.set(29,0xff00);
            check(nt::ee::run(nt_ee_generated_run,state,memory,3,services)==nt::ee::Stop::budget && state.pc==0x3a0000);
            std::uint32_t line; std::memcpy(&line,ram.data()+0x6000,4); check(line==2 && !memory.pending_interrupts);
            timer_now=edge(487); poll(); check((platform.graphics.read(0x12001000).value&0x2008)==0x2008 && !memory.pending_interrupts);
            platform.graphics.write(0x12001010,0x7700); poll(); check(platform.interrupts.read(0x1000f000).value==1);
            platform.interrupts.write(0x1000f000,1); poll(); check(!memory.pending_interrupts);
            platform.graphics.write(0x12001000,8); timer_now=edge(525); poll();
            check(platform.interrupts.read(0x1000f000).value==8 && !(platform.graphics.read(0x12001000).value&8));
            platform.interrupts.write(0x1000f000,8); timer_now=edge(1012); poll();
            check(edges_seen==std::vector<bool>({false,true,false}));
            check((platform.graphics.read(0x12001000).value&0x2008)==8);
            const auto before=platform.graphics.read(0x12001000).value;
            timer_now-=100; poll(); check(platform.graphics.read(0x12001000).value==before);
            check(configure(99)==nt::ee::Stop::unsupported_system && platform.graphics.mode()->standard==2);
            fail=true; check(configure(3)==nt::ee::Stop::unsupported_system && platform.graphics.mode()->standard==2);
            fail=false; platform.interrupts.write(0x1000f000,15);
            const auto epoch=timer_now;
            check(configure(3,0)==nt::ee::Stop::budget && platform.graphics.read(0x12000020).value==1);
            timer_now=epoch+576*32000ULL; poll(); check(platform.interrupts.read(0x1000f000).value==4);
            timer_now=epoch+625*32000ULL*1000000; poll();
            check(edges_seen.size()<300 && platform.graphics.edge_overflow());
            check((platform.interrupts.read(0x1000f000).value&12)==12);
            check(platform.graphics.write(0x12001000,0x200)==nt::ee::Stop::budget && !platform.graphics.mode());
            platform.interrupts.write(0x1000f000,15); timer_now+=1000000000; poll(); check(!memory.pending_interrupts);
        }
        {
            nt::ee::Memory memory; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::runtime::State state;
            const auto write=[&](unsigned address,unsigned value) {
                state.pc=0x360000; state.set(4,address); state.set(8,value);
                return nt_ee_generated_run(&state,&memory,1);
            };
            const auto read=[&](unsigned address) {
                state.pc=0x360008; state.set(4,address);
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
                return static_cast<std::uint32_t>(state.gpr[8].lo);
            };
            for (const auto base:{0x10003800U,0x10003c00U}) {
                check(read(base)==0 && read(base+0x60)==0);
                check(write(base+0x20,~0U)==nt::ee::Stop::budget);
                check(write(base+0x30,0x12345678)==nt::ee::Stop::budget);
                check(write(base+0x50,~0U)==nt::ee::Stop::budget);
                check(write(base+0x70,0xabcdef01)==nt::ee::Stop::budget);
                for (unsigned lane=0;lane<8;++lane) check(write(base+0x100+lane*16,lane+100)==nt::ee::Stop::budget);
                check(write(base+0x10,6)==nt::ee::Stop::budget && read(base)==0x300 && !read(base+0x10));
                check(write(base+0x10,8)==nt::ee::Stop::budget && read(base)==0 && read(base+0x20)==7);
                check(write(base+0x10,7)==nt::ee::Stop::budget && read(base)==0 && !read(base+0x20));
                check(read(base+0x30)==0x5678 && read(base+0x50)==3 && read(base+0x70)==0xabcdef01);
                for (unsigned lane=0;lane<8;++lane) check(read(base+0x100+lane*16)==lane+100);
            }
            check(write(0xb0003830,7)==nt::ee::Stop::budget && read(0x10003830)==7 && read(0x10003c30)==0x5678);
            check(write(0x10003c00,0x800000)==nt::ee::Stop::unsupported_system && read(0x10003c00)==0);
            check(write(0x10003010,~0U)==nt::ee::Stop::budget && write(0x10003000,8)==nt::ee::Stop::budget && read(0x10003020)==13);
            check(write(0x10003000,9)==nt::ee::Stop::budget && !read(0x10003020) && read(0x10003830)==7);
            for (const auto address:{0x10003840U,0x100038f0U,0x10003020U,0x10004000U,0x10005000U,0x10006000U}) {
                const auto retired=state.executed;
                check(write(address,0)==nt::ee::Stop::unsupported_system && state.executed==retired && state.pc==0x360000);
            }
            check(!memory.pending_interrupts && !platform.completions.status());
        }
        {
            std::array<std::uint8_t,64> ram{}; ram[0]=17; ram[16]=42;
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            struct Sink { unsigned calls=0,channel=99; std::size_t bytes=0; unsigned first=0; nt::ee::Stop result=nt::ee::Stop::unsupported_system; } sink;
            nt::runtime::State state;
            const auto write=[&](unsigned address,unsigned value) {
                state.pc=0x360000; state.set(4,address); state.set(8,value);
                return nt_ee_generated_run(&state,&memory,1);
            };
            const auto read=[&](unsigned address) {
                state.pc=0x360008; state.set(4,address);
                check(nt_ee_generated_run(&state,&memory,1)==nt::ee::Stop::budget);
                return static_cast<unsigned>(state.gpr[8].lo);
            };
            check(write(0x10008020,1)==nt::ee::Stop::budget);
            check(write(0x10008000,0x101)==nt::ee::Stop::unsupported_system && !platform.completions.status());
            platform.packet_submission.consumer={&sink,[](void* context,unsigned channel,std::span<const std::uint8_t> bytes) {
                auto& sink=*static_cast<Sink*>(context); ++sink.calls;
                if (sink.result==nt::ee::Stop::budget) {
                    sink.channel=channel; sink.bytes=bytes.size(); sink.first=bytes.empty() ? 0 : bytes.front();
                }
                return sink.result;
            }};
            check(write(0x10008000,0x101)==nt::ee::Stop::unsupported_system && read(0x10008020)==1 && !platform.completions.status());
            sink.result=nt::ee::Stop::budget;
            check(read(0x1000e000)==0 && read(0x1000f520)==0x1201);
            check(write(0x10008000,0x101)==nt::ee::Stop::unsupported_system && !platform.completions.status());
            check(write(0x1000e000,1)==nt::ee::Stop::budget);
            sink.result=nt::ee::Stop::unsupported_system;
            const auto rejected_calls=sink.calls;
            check(write(0x10008000,0x101)==nt::ee::Stop::unsupported_system && sink.calls==rejected_calls+1 && read(0x10008020)==1 && !platform.completions.status());
            sink.result=nt::ee::Stop::budget;
            check(write(0x1000e000,3)==nt::ee::Stop::unsupported_system && read(0x1000e000)==1);
            check(write(0x1000e020,0x7fffffff)==nt::ee::Stop::budget && read(0x1000e020)==0x03ff03ff);
            check(write(0x1000e020,0x80000000)==nt::ee::Stop::unsupported_system && read(0x1000e020)==0x03ff03ff);
            check(write(0x1000f590,0x11201)==nt::ee::Stop::budget && read(0x1000f520)==0x11201);
            const auto suspended_calls=sink.calls;
            check(write(0x10008000,0x101)==nt::ee::Stop::unsupported_system && sink.calls==suspended_calls);
            check(write(0x1000f590,0x1201)==nt::ee::Stop::budget && read(0x1000f590)==0x1201);
            check(write(0x1000f520,0)==nt::ee::Stop::unsupported_system && read(0x1000f520)==0x1201);
            for (unsigned channel=0;channel<3;++channel) {
                const auto base=0x10008000+channel*0x1000;
                check(write(base+0x80,0)==nt::ee::Stop::budget && read(base+0x80)==0);
                check(write(base+0x80,1)==nt::ee::Stop::unsupported_system && read(base+0x80)==0);
                check(write(base+0x10,16)==nt::ee::Stop::budget && write(base+0x20,1)==nt::ee::Stop::budget);
                check(write(base,0x101)==nt::ee::Stop::budget);
                check(sink.channel==channel && sink.bytes==16 && sink.first==42);
                check(read(base)==1 && read(base+0x10)==32 && read(base+0x20)==0);
                check(platform.completions.status()&(1U<<channel));
            }
            platform.completions.acknowledge(7);
            check(write(0x10008020,1)==nt::ee::Stop::budget);
            const auto calls=sink.calls;
            check(write(0x10008000,0x105)==nt::ee::Stop::unsupported_system && sink.calls==calls);
            check(write(0x10008010,63)==nt::ee::Stop::budget);
            check(write(0x10008000,0x101)==nt::ee::Stop::memory_fault && sink.calls==calls && !platform.completions.status());
            memory.scratchpad[0]=77;
            check(write(0x10008010,0x80000000)==nt::ee::Stop::budget);
            check(write(0x10008000,0x101)==nt::ee::Stop::budget && sink.first==77);
            platform.completions.acknowledge(7);
            check(write(0x10008010,0x80003ff0)==nt::ee::Stop::budget && write(0x10008020,2)==nt::ee::Stop::budget);
            check(write(0x10008000,0x101)==nt::ee::Stop::memory_fault && !platform.completions.status());
        }
        {
            std::array<std::uint8_t,1024> ram{};
            nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
            nt::runtime::State state;
            const auto put=[&](unsigned at,std::uint32_t value) { std::memcpy(ram.data()+at,&value,4); };
            const auto write=[&](unsigned address,unsigned value) {
                state.pc=0x360000; state.set(4,address); state.set(8,value);
                return nt_ee_generated_run(&state,&memory,1);
            };
            const auto submit=[&](unsigned control=0x145) {
                check(write(0x10008030,0)==nt::ee::Stop::budget);
                return write(0x10008000,control);
            };
            check(write(0x1000e000,1)==nt::ee::Stop::budget);
            for (const unsigned kind:{0U,1U,2U,3U,7U}) {
                ram.fill(0); platform.completions.acknowledge(1);
                const auto external=kind==0 || kind==3;
                put(0,(kind<<28)|1); put(4,kind==2 ? 64 : 128);
                put(8,0x01000404); put(12,0x0700002a);
                put(external ? 128 : 16,0x05000002);
                if (kind==1) put(32,0x70000000);
                if (kind==2) put(64,0x70000000);
                if (kind==3) put(16,0x70000000);
                check(submit()==nt::ee::Stop::budget);
                check(platform.packet_setup.read(0x10003840).value==0x404 && platform.packet_setup.read(0x10003830).value==42);
                check(platform.packet_setup.read(0x10003850).value==2 && (platform.completions.status()&1));
                check(!(platform.packet_submission.read(0x10008000).value&0x100));
            }
            // Whole-batch validation: a later unsupported upload cannot commit a
            // preceding MARK, and rejected batches publish no completion.
            ram.fill(0); platform.completions.acknowledge(1);
            put(0,0x70000001); put(8,0x07000063); put(16,0x4a010000);
            struct Capture { unsigned calls=0,unit=9,destination=9,packets=0; std::size_t size=0,packet_size=0; } capture;
            platform.packet_setup.upload_context=&capture;
            platform.packet_setup.uncompiled_packet=[](void* context,unsigned,std::span<const std::uint8_t> bytes) {
                auto& result=*static_cast<Capture*>(context); ++result.packets; result.packet_size=bytes.size();
            };
            platform.packet_setup.uncompiled_upload=[](void* context,unsigned unit,unsigned destination,std::span<const std::uint8_t> bytes) {
                auto& result=*static_cast<Capture*>(context); ++result.calls;
                result.unit=unit; result.destination=destination; result.size=bytes.size();
            };
            const auto previous_source=platform.packet_submission.read(0x10008010).value;
            check(submit()==nt::ee::Stop::unsupported_system && platform.packet_setup.rejected_command()==0x4a);
            check(platform.packet_setup.read(0x10003830).value==42 && !platform.completions.status());
            check(capture.calls==1 && capture.unit==0 && capture.destination==0 && capture.size==8);
            check(capture.packets==1 && capture.packet_size==24);
            put(16,0x4a000000);
            check(submit()==nt::ee::Stop::invalid_state && capture.calls==1 && !platform.completions.status());
            check(capture.packets==1);
            check(platform.packet_submission.read(0x10008010).value==previous_source);
            // Cyclic chains and bad extents are rejected before any consumer call.
            ram.fill(0); put(0,0x20000000);
            check(submit()==nt::ee::Stop::invalid_state && !platform.completions.status());
            put(0,0x00000001); put(4,1024);
            check(submit()==nt::ee::Stop::memory_fault && !platform.completions.status());
            put(0,0x50000000);
            check(submit()==nt::ee::Stop::unsupported_system && !platform.completions.status());
            // IRQ termination excludes the following bad descriptor.
            put(0,0x90000000); put(8,0x07000033); put(16,0x50000000);
            check(submit(0x1c5)==nt::ee::Stop::budget && platform.packet_setup.read(0x10003830).value==51);
            // Truncated multiword configuration is atomic too.
            const std::array<std::uint32_t,2> incomplete{0x07000044,0x30000000};
            check(platform.packet_setup.consume(0,{reinterpret_cast<const std::uint8_t*>(incomplete.data()),sizeof(incomplete)},false)==nt::ee::Stop::invalid_state);
            check(platform.packet_setup.read(0x10003830).value==51);
        }
        run_ee_batch_tests();
        run_ee_file_rpc_tests();
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
