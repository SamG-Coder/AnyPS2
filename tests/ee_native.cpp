#include <nt/ee_services.hpp>
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
int main() {
    try {
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
            std::array<std::uint8_t,0x2000> ram{}; ram.fill(0xcc);
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
            std::array<std::uint8_t,0x2000> ram{}; ram.fill(0xcc);
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
        for (const auto slice : {1U,64U}) {
            std::array<std::uint8_t,0x10000> ram{}; ram.fill(0xcc);
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
            std::array<std::uint8_t,0x10000> ram{}; ram.fill(0xcc); const auto original=ram;
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
            std::array<std::uint8_t,0x10000> ram{};
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
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
