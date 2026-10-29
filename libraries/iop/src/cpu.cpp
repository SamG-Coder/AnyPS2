#include <nt/iop_cpu.hpp>
#include <nt/iop_memory.hpp>
#include <nt/word_merge.hpp>
#include <bit>
#include <optional>

namespace nt::iop {
namespace {
std::uint32_t signed16(std::uint32_t word) { return (word&0x8000) ? word|0xffff0000U : word&0xffff; }
std::int32_t signed32(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::uint32_t arithmetic_right(std::uint32_t value,unsigned amount) {
    return !amount ? value : (value>>amount)|((value&0x80000000U) ? (~0U<<(32-amount)) : 0);
}
}
CpuResult run(Cpu& cpu,std::span<std::uint8_t> ram,std::uint64_t budget,Devices* devices,HostCalls* calls) {
    while (budget--) {
        const bool interrupt=devices && devices->interrupt_pending();
        cpu.cause=(cpu.cause&~0x400U)|(interrupt ? 0x400U : 0);
        if (interrupt && (cpu.status&0x401)==0x401) {
            // No firmware vector is mapped. Keep this boundary resumable.
            if (cpu.status&0x400000) return {CpuStop::interrupt_pending,cpu.pc,0};
            auto redirected=cpu;
            const auto routed=calls ? calls->interrupt(cpu,redirected,ram) : CallResult{};
            if (routed.state==CallState::pending) return {CpuStop::interrupt_pending,cpu.pc,0};
            if (routed.state==CallState::address_fault) return {CpuStop::address_fault,cpu.pc,0,routed.address};
            if (routed.state==CallState::transferred) cpu=redirected;
            else {
                cpu.epc=cpu.delay_slot ? cpu.pc-4 : cpu.pc;
                cpu.cause=(cpu.cause&~0x8000007fU)|(cpu.delay_slot ? 0x80000000U : 0);
                cpu.status=(cpu.status&~0x3fU)|((cpu.status&15)<<2);
                if (cpu.load_register) cpu.registers[cpu.load_register]=cpu.load_value;
                cpu.load_register=0;
                cpu.delay_slot=false;
                cpu.pc=0x80000080;
                ++cpu.interrupt_depth;
            }
        }
        const auto fetch=ram_offset(cpu.pc,4,ram.size());
        if (!fetch) return {CpuStop::address_fault,cpu.pc,0,cpu.pc};
        const auto word=le32(ram,*fetch), op=word>>26, rs=(word>>21)&31, rt=(word>>16)&31;
        const auto rd=(word>>11)&31, shift=(word>>6)&31, fn=word&63;
        const auto a=rs ? cpu.registers[rs] : 0, b=rt ? cpu.registers[rt] : 0;
        const auto immediate=signed16(word);
        auto next=cpu;
        next.registers[0]=0;
        next.pc=cpu.delay_slot ? cpu.branch_target : cpu.pc+4;
        next.delay_slot=false; next.load_register=0;
        unsigned written=0;
        bool transferred=false;
        std::optional<CpuStop> failure;
        std::optional<std::uint32_t> fault_address;
        const auto write=[&](unsigned reg,std::uint32_t value) { if (reg) { next.registers[reg]=value; written=reg; } };
        const auto branch=[&](std::uint32_t target) {
            if (cpu.delay_slot) { failure=CpuStop::branch_in_delay; return; }
            next.delay_slot=true; next.branch_target=target;
        };
        const auto conditional=[&](bool taken) { branch(taken ? cpu.pc+4+(immediate<<2) : cpu.pc+8); };
        const auto add=[&](unsigned reg,std::uint32_t left,std::uint32_t right,bool trap,bool subtract) {
            const auto value=subtract ? left-right : left+right;
            if (trap && ((subtract ? (left^right) : ~(left^right))&(left^value)&0x80000000U))
                failure=CpuStop::overflow;
            else write(reg,value);
        };
        if (op==0) {
            switch (fn) {
            case 0: write(rd,b<<shift); break;
            case 2: write(rd,b>>shift); break;
            case 3: write(rd,arithmetic_right(b,shift)); break;
            case 4: write(rd,b<<(a&31)); break;
            case 6: write(rd,b>>(a&31)); break;
            case 7: write(rd,arithmetic_right(b,a&31)); break;
            case 8: branch(a); break;
            case 9: branch(a); write(rd,cpu.pc+8); break;
            case 12: {
                if (!calls || word!=12 || cpu.delay_slot) { failure=CpuStop::unsupported; break; }
                auto result=calls->invoke(cpu.pc,{cpu.registers[4],cpu.registers[5],cpu.registers[6],cpu.registers[7]},ram,cpu.interrupt_depth!=0);
                if (result.state==CallState::unsupported) result=calls->syscall(cpu,next,ram);
                if (result.state==CallState::returned) write(2,result.value);
                else if (result.state==CallState::transferred) transferred=true;
                else if (result.state==CallState::pending) failure=CpuStop::interrupt_pending;
                else if (result.state==CallState::address_fault) { failure=CpuStop::address_fault; fault_address=result.address; }
                else failure=CpuStop::unsupported;
                break;
            }
            case 16: write(rd,cpu.hi); break;
            case 17: next.hi=a; break;
            case 18: write(rd,cpu.lo); break;
            case 19: next.lo=a; break;
            case 24: case 25: {
                const auto product=fn==24 ? static_cast<std::uint64_t>(std::int64_t(signed32(a))*signed32(b)) : std::uint64_t(a)*b;
                next.lo=static_cast<std::uint32_t>(product); next.hi=static_cast<std::uint32_t>(product>>32);
                break;
            }
            case 26:
                if (!b) { next.lo=signed32(a)<0 ? 1U : 0xffffffffU; next.hi=a; }
                else if (a==0x80000000U && b==0xffffffffU) { next.lo=a; next.hi=0; }
                else { next.lo=static_cast<std::uint32_t>(signed32(a)/signed32(b)); next.hi=static_cast<std::uint32_t>(signed32(a)%signed32(b)); }
                break;
            case 27:
                next.lo=b ? a/b : 0xffffffffU; next.hi=b ? a%b : a;
                break;
            case 32: case 33: add(rd,a,b,fn==32,false); break;
            case 34: case 35: add(rd,a,b,fn==34,true); break;
            case 36: write(rd,a&b); break;
            case 37: write(rd,a|b); break;
            case 38: write(rd,a^b); break;
            case 39: write(rd,~(a|b)); break;
            case 42: write(rd,signed32(a)<signed32(b)); break;
            case 43: write(rd,a<b); break;
            default: failure=CpuStop::unsupported; break;
            }
        } else if (op==1) {
            if (rt!=0 && rt!=1 && rt!=16 && rt!=17) failure=CpuStop::unsupported;
            else {
                conditional((rt&1) ? signed32(a)>=0 : signed32(a)<0);
                if (rt&16) write(31,cpu.pc+8);
            }
        } else if (op==2 || op==3) {
            branch(((cpu.pc+4)&0xf0000000U)|((word&0x03ffffff)<<2));
            if (op==3) write(31,cpu.pc+8);
        } else if (op>=4 && op<=7) {
            conditional(op==4 ? a==b : op==5 ? a!=b : op==6 ? signed32(a)<=0 : signed32(a)>0);
        } else if (op==8 || op==9) add(rt,a,immediate,op==8,false);
        else if (op==10) write(rt,signed32(a)<signed32(immediate));
        else if (op==11) write(rt,a<immediate);
        else if (op==12) write(rt,a&(word&0xffff));
        else if (op==13) write(rt,a|(word&0xffff));
        else if (op==14) write(rt,a^(word&0xffff));
        else if (op==15) write(rt,word<<16);
        else if (op==16) {
            if (word==0x42000010) {
                const auto restored=(cpu.status&~15U)|((cpu.status>>2)&15);
                if (restored&2) failure=CpuStop::unsupported;
                else { next.status=restored; if (next.interrupt_depth) --next.interrupt_depth; }
            } else if ((word&0x7ff) || (rs!=0 && rs!=4)) failure=CpuStop::unsupported;
            else if (rs==0 && (rd==12 || rd==13 || rd==14 || rd==15)) {
                next.load_register=rt;
                next.load_value=rd==12 ? cpu.status : rd==13 ? cpu.cause : rd==14 ? cpu.epc : 0x1f;
            } else if (rs!=4 || rd!=12 || (b&0x00020002U)) failure=CpuStop::unsupported;
            else next.status=b;
        }
        else if (op==34 || op==38 || op==42 || op==46) {
            const auto guest=a+immediate, aligned=guest&~3U;
            const auto address=ram_offset(aligned,4,ram.size());
            const bool load=op==34 || op==38, left=op==34 || op==42;
            if (!address) { failure=CpuStop::address_fault; fault_address=guest; }
            else if (cpu.status&0x10000) {
                if (load) { failure=CpuStop::unsupported; fault_address=guest; }
                // Isolated stores follow the ordinary store path: RAM is untouched.
            } else {
                const auto memory=le32(ram,*address), offset=guest&3;
                if (load) {
                    // A paired partial load merges with its pending result while
                    // ordinary consumers still observe the architectural delay.
                    const auto original=rt && cpu.load_register==rt ? cpu.load_value : b;
                    next.load_register=rt;
                    next.load_value=nt::merge_load(original,memory,offset,left);
                } else {
                    const auto value=nt::merge_load(memory,b,3-offset,!left);
                    for (unsigned i=0;i<4;++i) ram[*address+i]=static_cast<std::uint8_t>(value>>(8*i));
                }
            }
        }
        else if (op==32 || op==33 || op==35 || op==36 || op==37 || op==40 || op==41 || op==43) {
            const unsigned size=(op==32 || op==36 || op==40) ? 1 : (op==33 || op==37 || op==41) ? 2 : 4;
            const auto address=ram_offset(a+immediate,size,ram.size());
            const auto guest=a+immediate, segment=guest&0xe0000000U;
            const bool device_access=devices && guest%size==0;
            const auto device_address=(segment==0x80000000U || segment==0xa0000000U) ? guest&0x1fffffffU : guest;
            if (op<40) {
                std::uint32_t value=0;
                if (address && (cpu.status&0x10000)) { failure=CpuStop::unsupported; fault_address=guest; }
                else if (address) for (unsigned i=0;i<size;++i) value|=std::uint32_t(ram[*address+i])<<(8*i);
                else {
                    const auto loaded=device_access ? devices->read(device_address,size) : std::nullopt;
                    if (loaded) value=*loaded;
                    else { failure=CpuStop::address_fault; fault_address=guest; }
                }
                if (op==32 && (value&0x80)) value|=0xffffff00U;
                if (op==33) value=signed16(value);
                next.load_register=rt; next.load_value=value;
            } else if (address) {
                // Isolated stores must never corrupt main RAM. Cache data/tag
                // reads are unsupported; instruction fetch remains coherent.
                if (!(cpu.status&0x10000))
                    for (unsigned i=0;i<size;++i) ram[*address+i]=static_cast<std::uint8_t>(b>>(8*i));
            } else if (!device_access || !devices->write(device_address,size,b)) {
                failure=CpuStop::address_fault; fault_address=guest;
            }
        } else failure=CpuStop::unsupported;
        if (failure) return {*failure,cpu.pc,word,fault_address};
        // The instruction reads old registers. A newer write/load to the same
        // register cancels the prior delayed load; other loads retire here.
        if (!transferred && cpu.load_register && cpu.load_register!=written && cpu.load_register!=next.load_register)
            next.registers[cpu.load_register]=cpu.load_value;
        next.registers[0]=0;
        ++next.executed;
        cpu=next;
        if (devices) devices->instruction_retired();
    }
    return {CpuStop::budget,cpu.pc,0};
}
}
