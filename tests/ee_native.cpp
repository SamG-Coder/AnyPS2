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
            state={}; state.pc=0x147c; state.gpr[8].lo=0x80000000;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_system &&
                  !state.executed && state.pc==0x147c && memory.performance.control==0x12340);
            state.pc=0x14a4;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && memory.performance.control==0x12340);
            memory.performance.control=0x80000000; state={}; state.pc=0x1488; state.gpr[11].lo=123;
            check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::unsupported_system &&
                  !state.executed && state.gpr[11].lo==123);
        }
        for (unsigned variant=0;variant<4;++variant) {
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
                    nt::runtime::State state; state.pc=0x14ac+variant*8;
                    state.gpr[8].lo=0x1234567800000000ULL|a; state.gpr[9].lo=0xfedcba9800000000ULL|b;
                    state.gpr[10]={77,88}; state.hi1=111; state.lo1=222;
                    check(run(&state,8)==nt::ee::Stop::syscall && state.executed==1 && state.lo==lo && state.hi==hi &&
                          state.gpr[10].lo==(variant<2 ? lo : 77) && state.gpr[10].hi==88 && state.hi1==111 && state.lo1==222);
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
                    std::array<std::uint8_t,24> ram; ram.fill(0xcc); auto expected=ram;
                    if (!side) for (unsigned n=0;n<=offset;++n) expected[n]=source[7-offset+n];
                    else for (unsigned n=offset;n<8;++n) expected[n]=source[n-offset];
                    nt::ee::Memory memory{ram.data(),ram.size()}; nt::runtime::State state; state.pc=0x1654+side*8;
                    state.gpr[4].lo=0xa0000000+offset; state.gpr[8]={0x8877665544332211ULL,99};
                    check(nt_ee_generated_run(&state,&memory,8)==nt::ee::Stop::syscall && state.executed==1 && ram==expected &&
                          state.gpr[8].lo==0x8877665544332211ULL && state.gpr[8].hi==99);
                }
                std::array<std::uint8_t,24> ram; ram.fill(0xcc); auto expected=ram;
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
            std::array<std::uint8_t,8> ram; ram.fill(0xcc); nt::ee::Memory memory{ram.data(),ram.size()};
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
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
