#include "ee_batch_fixture.hpp"
#include <nt/ee_native.hpp>
#include <nt/ee_vector_math.hpp>
#include <algorithm>
#include <bit>
#include <cfenv>
#include <limits>
#include <source_location>
#include <string>

extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" unsigned nt_ee_vector_add(nt::ee::Memory*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
extern "C" unsigned nt_ee_vector_multiply(nt::ee::Memory*,unsigned,unsigned,unsigned,unsigned,unsigned);
extern "C" unsigned nt_ee_vector_multiply_accumulate(nt::ee::Memory*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
namespace {
using nt::runtime::State;
using nt::runtime::Register;
using nt::ee::Stop;
using Words=std::array<std::uint32_t,4>;
std::string context;
void check(bool value,std::source_location source=std::source_location::current()) {
    if (!value) throw std::runtime_error("EE batch "+context+" at line "+std::to_string(source.line()));
}
Stop execute(State& state,nt::ee::Memory& memory,std::uint64_t budget=8) {
    return nt_ee_generated_run(&state,&memory,budget);
}
Stop execute(State& state,std::uint64_t budget=8) {
    nt::ee::Memory memory;
    return execute(state,memory,budget);
}
std::uint64_t lane(Register value,unsigned index,unsigned width) {
    const auto bits=index*width;
    return ((bits<64 ? value.lo : value.hi)>>(bits%64))&((1ULL<<width)-1);
}
std::int64_t signed_lane(std::uint64_t value,unsigned width) {
    return std::int64_t(value)-((value&(1ULL<<(width-1))) ? (1LL<<width) : 0);
}
void put_lane(Register& value,unsigned index,unsigned width,std::uint64_t bits) {
    auto& half=index*width<64 ? value.lo : value.hi;
    const auto shift=(index*width)%64;
    const auto mask=((1ULL<<width)-1)<<shift;
    half=(half&~mask)|((bits<<shift)&mask);
}
std::uint64_t sign_word(std::uint32_t value) {
    return value&0x80000000U ? std::uint64_t(value)|0xffffffff00000000ULL : value;
}
void check_register(Register actual,Register expected) {
    check(actual.lo==expected.lo && actual.hi==expected.hi);
}
void packed_arithmetic() {
    const std::array<Register,5> samples{{
        {0x7fffffff80000000ULL,0x00010000ffffffffULL},
        {0x7fff80007f800001ULL,0xffff0001ff017f80ULL},
        {0x0706050403020100ULL,0x0f0e0d0c0b0a0908ULL},
        {0xfefdfcfbfaf9f8f7ULL,0xf6f5f4f3f2f1f0efULL},
        {0xffffffffffffffffULL,0x8000000080000000ULL}}};
    for (std::size_t index=0;index<ee_batch::packed.size();++index) {
        const auto fixture=ee_batch::packed[index];
        const auto name=fixture.name;
        if (name.starts_with("phm") || name=="qfsrv" || (name.starts_with("pm") && !name.starts_with("pmax") && !name.starts_with("pmin")) || name.starts_with("pd") || name.starts_with("madd")) continue;
        for (unsigned sample=0;sample<samples.size();++sample) for (unsigned variant=0;variant<4;++variant) {
            context=std::string(name)+" sample="+std::to_string(sample)+" destination="+std::to_string(variant);
            State state; state.pc=ee_batch::packed_pc(index,variant);
            state.gpr[8]=samples[sample]; state.gpr[9]=samples[(sample+1)%samples.size()];
            state.gpr[10]={55,66}; state.gpr[0]={77,88};
            state.hi=0x1234; state.lo=0x5678; state.hi1=0xabcd; state.lo1=0xef01;
            const auto left=state.gpr[8],right=state.gpr[9]; Register expected;
            const bool permutation=name=="pexeh" || name=="prevh" || name=="pexew" || name=="prot3w" || name=="pexch" || name=="pexcw" || name=="pinth" || name=="pinteh";
            if (permutation) {
                const unsigned width=name=="pexew" || name=="prot3w" || name=="pexcw" ? 32 : 16;
                std::array<unsigned,8> order{};
                if (name=="pexeh") order={2,1,0,3,6,5,4,7};
                if (name=="prevh") order={3,2,1,0,7,6,5,4};
                if (name=="pexch") order={0,2,1,3,4,6,5,7};
                if (name=="pexew") order={2,1,0,3};
                if (name=="prot3w") order={1,2,0,3};
                if (name=="pexcw") order={0,2,1,3};
                if (name=="pinth") order={0,12,1,13,2,14,3,15};
                if (name=="pinteh") order={0,8,2,10,4,12,6,14};
                for (unsigned n=0;n<128/width;++n)
                    put_lane(expected,n,width,lane(order[n]<128/width ? right : left,order[n]%(128/width),width));
            } else if (name.ends_with("vw")) {
                for (unsigned n=0;n<2;++n) {
                    const auto shift=unsigned(lane(left,n*2,32)&31);
                    const auto input=std::uint32_t(lane(right,n*2,32));
                    const auto value=name=="psllvw" ? input<<shift : name=="psrlvw" ? input>>shift :
                        std::uint32_t(signed_lane(input,32)>>shift);
                    (n ? expected.hi : expected.lo)=sign_word(value);
                }
            } else {
                const unsigned width=name.back()=='b' ? 8 : name.back()=='h' ? 16 : 32;
                const auto maximum=(1ULL<<width)-1;
                const auto signed_max=std::int64_t(maximum>>1),signed_min=-signed_max-1;
                for (unsigned n=0;n<128/width;++n) {
                    const auto a=lane(left,n,width),b=lane(right,n,width);
                    const auto x=signed_lane(a,width),y=signed_lane(b,width);
                    std::int64_t result=0;
                    if (name.starts_with("padd")) result=std::int64_t(a+b);
                    if (name.starts_with("psub")) result=std::int64_t(a)-std::int64_t(b);
                    if (name.starts_with("padds")) result=std::clamp(x+y,signed_min,signed_max);
                    if (name.starts_with("psubs")) result=std::clamp(x-y,signed_min,signed_max);
                    if (name.starts_with("paddu")) result=std::int64_t(std::min(a+b,maximum));
                    if (name.starts_with("psubu")) result=std::int64_t(a>b ? a-b : 0);
                    if (name.starts_with("pcgt")) result=x>y ? -1 : 0;
                    if (name.starts_with("pmax")) result=std::max(x,y);
                    if (name.starts_with("pmin")) result=std::min(x,y);
                    if (name.starts_with("pabs")) result=std::min(y<0 ? -y : y,signed_max);
                    if (name=="padsbh") result=n<4 ? x-y : x+y;
                    if (name.starts_with("psll")) result=std::int64_t(b<<(31&(width-1)));
                    if (name.starts_with("psrl")) result=std::int64_t(b>>(31&(width-1)));
                    if (name.starts_with("psra")) result=y>>(31&(width-1));
                    put_lane(expected,n,width,std::uint64_t(result));
                }
            }
            check(execute(state)==Stop::syscall && state.executed==1);
            const auto destination=ee_batch::destinations[variant];
            if (destination) check_register(state.gpr[destination],expected);
            check_register(state.gpr[0],{77,88});
            check(state.hi==0x1234 && state.lo==0x5678 && state.hi1==0xabcd && state.lo1==0xef01);
        }
    }
}
void integer_products_and_funnels() {
    for (std::size_t index=0;index<ee_batch::packed.size();++index) {
        const auto name=ee_batch::packed[index].name;
        if (name=="qfsrv") {
            for (unsigned shift=0;shift<=16;++shift) for (unsigned variant=0;variant<4;++variant) {
                context="qfsrv shift="+std::to_string(shift);
                State state; state.pc=ee_batch::packed_pc(index,variant); state.sa=shift;
                state.gpr[8]={0x1716151413121110ULL,0x1f1e1d1c1b1a1918ULL};
                state.gpr[9]={0x0706050403020100ULL,0x0f0e0d0c0b0a0908ULL};
                state.gpr[10]={99,88}; state.gpr[0]={77,66};
                const auto before=state;
                check(execute(state)==(shift==16 ? Stop::unsupported_system : Stop::syscall));
                if (shift==16) { check(state.pc==before.pc && state.executed==0); check_register(state.gpr[10],before.gpr[10]); }
                else if (const auto destination=ee_batch::destinations[variant])
                    for (unsigned n=0;n<16;++n) check(lane(state.gpr[destination],n,8)==shift+n);
                check_register(state.gpr[0],{77,66});
            }
        } else if (name=="pmfhi" || name=="pmflo" || name=="pmthi" || name=="pmtlo") {
            for (unsigned variant=0;variant<4;++variant) {
                context=std::string(name); State state; state.pc=ee_batch::packed_pc(index,variant);
                state.gpr[8]={0x0123456789abcdefULL,0xfedcba9876543210ULL};
                state.gpr[0]={33,44}; state.hi=11; state.hi1=22; state.lo=55; state.lo1=66;
                check(execute(state)==Stop::syscall && state.executed==1);
                if (name=="pmthi") check(state.hi==state.gpr[8].lo && state.hi1==state.gpr[8].hi && state.lo==55 && state.lo1==66);
                if (name=="pmtlo") check(state.lo==state.gpr[8].lo && state.lo1==state.gpr[8].hi && state.hi==11 && state.hi1==22);
                if (const auto destination=ee_batch::destinations[variant]; destination && (name=="pmfhi" || name=="pmflo"))
                    check_register(state.gpr[destination],name=="pmfhi" ? Register{11,22} : Register{55,66});
                check_register(state.gpr[0],{33,44});
            }
        } else if (name.starts_with("madd") || name=="pmultw" || name=="pmultuw" || name=="pdivw" || name=="pdivuw" || name=="pmadduw") {
            for (const auto pair : std::array<std::pair<std::uint32_t,std::uint32_t>,6>{{
                {7,3},{0x80000000U,0xffffffffU},{0xffffffffU,0},{0x80000000U,0},{0,0},{0xffffffffU,0xffffffffU}}})
                for (unsigned variant=0;variant<4;++variant) {
                    context=std::string(name)+" destination="+std::to_string(variant);
                    State state; state.pc=ee_batch::packed_pc(index,variant); state.gpr[0]={33,44};
                    state.gpr[8]={pair.first,9}; state.gpr[9]={pair.second,2}; state.gpr[10]={99,88};
                    state.lo=0xffffffffU; state.hi=0xffffffffU; state.lo1=5; state.hi1=7;
                    const auto before=state;
                    const bool packed=name.front()=='p',secondary=name.ends_with('1');
                    const bool divide=name.starts_with("pdiv"),signed_op=name=="madd" || name=="madd1" || name=="pmultw" || name=="pdivw";
                    const bool accumulate=name.starts_with("madd") || name=="pmadduw";
                    std::array<std::uint64_t,2> full{},expected_lo{},expected_hi{};
                    for (unsigned n=0;n<(packed ? 2U : 1U);++n) {
                        const auto a=n ? 9U : pair.first,b=n ? 2U : pair.second;
                        if (!divide) {
                            full[n]=signed_op ? std::uint64_t(signed_lane(a,32)*signed_lane(b,32)) : std::uint64_t(a)*b;
                            if (accumulate) full[n]+=(secondary || n ? (7ULL<<32)|5U : ~0ULL);
                            expected_lo[n]=sign_word(std::uint32_t(full[n])); expected_hi[n]=sign_word(std::uint32_t(full[n]>>32));
                        } else {
                            const auto x=signed_op ? signed_lane(a,32) : std::int64_t(a);
                            const auto y=signed_op ? signed_lane(b,32) : std::int64_t(b);
                            expected_lo[n]=sign_word(std::uint32_t(y ? x/y : signed_op && x<0 ? 1 : -1));
                            expected_hi[n]=sign_word(std::uint32_t(y ? x%y : x));
                        }
                    }
                    check(execute(state)==Stop::syscall && state.executed==1);
                    check((secondary ? state.lo1 : state.lo)==expected_lo[0] && (secondary ? state.hi1 : state.hi)==expected_hi[0]);
                    if (packed) check(state.lo1==expected_lo[1] && state.hi1==expected_hi[1]);
                    else check((secondary ? state.lo : state.lo1)==(secondary ? before.lo : before.lo1) &&
                               (secondary ? state.hi : state.hi1)==(secondary ? before.hi : before.hi1));
                    const auto destination=ee_batch::destinations[variant];
                    if (destination && !divide) {
                        check(state.gpr[destination].lo==(packed ? full[0] : expected_lo[0]));
                        check(state.gpr[destination].hi==(packed ? full[1] : before.gpr[destination].hi));
                    }
                    check_register(state.gpr[0],{33,44});
                }
        }
    }
}
void accumulator_packing() {
    for (std::size_t index=0;index<ee_batch::packed.size();++index) {
        const auto fixture=ee_batch::packed[index];
        if (fixture.name!="pmfhl" && fixture.name!="pmthl") continue;
        for (unsigned variant=0;variant<4;++variant) {
            context=std::string(fixture.name)+" mode="+std::to_string((fixture.word>>6)&31);
            State state; state.pc=ee_batch::packed_pc(index,variant);
            state.lo=0x7fffffff80000001ULL; state.lo1=0xffff800000007fffULL;
            state.hi=0x8000000000000001ULL; state.hi1=0x00010000ffffffffULL;
            state.gpr[8]={0x1122334455667788ULL,0x99aabbccddeeff00ULL}; state.gpr[0]={77,88};
            const auto before=state; Register expected;
            const auto mode=(fixture.word>>6)&31;
            if (fixture.name=="pmfhl") {
                const std::array<std::uint32_t,4> low{0x80000001,0x7fffffff,0x7fff,0xffff8000};
                const std::array<std::uint32_t,4> high{1,0x80000000,0xffffffff,0x10000};
                if (mode<=1) for (unsigned n=0;n<4;++n) put_lane(expected,n,32,(n%2 ? high : low)[(n/2)*2+mode]);
                else if (mode==2) expected={0x7fffffff,0xffffffff80000000ULL};
                else {
                    const std::array<std::uint32_t,8> values{low[0],low[1],high[0],high[1],low[2],low[3],high[2],high[3]};
                    for (unsigned n=0;n<8;++n) {
                        const auto value=mode==3 ? std::int64_t(values[n]) : std::clamp(signed_lane(values[n],32),std::int64_t(-32768),std::int64_t(32767));
                        put_lane(expected,n,16,std::uint64_t(value));
                    }
                }
            }
            check(execute(state)==Stop::syscall && state.executed==1);
            if (fixture.name=="pmthl") {
                check(state.lo==0x7fffffff55667788ULL && state.hi==0x8000000011223344ULL &&
                      state.lo1==0xffff8000ddeeff00ULL && state.hi1==0x0001000099aabbccULL);
            } else {
                if (const auto destination=ee_batch::destinations[variant]) check_register(state.gpr[destination],expected);
                check(state.lo==before.lo && state.hi==before.hi && state.lo1==before.lo1 && state.hi1==before.hi1);
            }
            check_register(state.gpr[0],{77,88});
        }
    }
}
void halfword_products() {
    for (std::size_t index=0;index<ee_batch::packed.size();++index) {
        const auto name=ee_batch::packed[index].name;
        if (name!="pmulth" && name!="pmaddh" && name!="pmsubh" && name!="phmadh" && name!="phmsbh" && name!="pdivbw") continue;
        for (unsigned variant=0;variant<4;++variant) for (unsigned sample=0;sample<3;++sample) {
            context=std::string(name)+" sample="+std::to_string(sample);
            State state; state.pc=ee_batch::packed_pc(index,variant);
            state.gpr[8]={0x80007fff80000000ULL,0xffff000100027fffULL};
            state.gpr[9]={0x800080007fff0000ULL|std::array{3ULL,0ULL,0xffffULL}[sample],0xffff000200018000ULL};
            state.gpr[0]={77,88}; state.gpr[10]={99,100};
            state.lo=0x7fffffff7fffffffULL; state.hi=0x8000000080000000ULL;
            state.lo1=0x0102030405060708ULL; state.hi1=0xfefdfcfbfaf9f8f7ULL;
            const auto before=state; Register expected,expected_lo,expected_hi;
            if (name=="pdivbw") {
                const auto divisor=signed_lane(lane(before.gpr[9],0,16),16);
                for (unsigned n=0;n<4;++n) {
                    const auto dividend=signed_lane(lane(before.gpr[8],n,32),32);
                    put_lane(expected_lo,n,32,std::uint64_t(divisor ? dividend/divisor : dividend<0 ? 1 : -1));
                    put_lane(expected_hi,n,32,std::uint64_t(divisor ? dividend%divisor : dividend));
                }
            } else {
                std::array<std::uint32_t,8> products{};
                for (unsigned n=0;n<8;++n) products[n]=std::uint32_t(signed_lane(lane(before.gpr[8],n,16),16)*signed_lane(lane(before.gpr[9],n,16),16));
                const std::array<std::uint32_t,8> accumulators{
                    std::uint32_t(before.lo),std::uint32_t(before.lo>>32),std::uint32_t(before.hi),std::uint32_t(before.hi>>32),
                    std::uint32_t(before.lo1),std::uint32_t(before.lo1>>32),std::uint32_t(before.hi1),std::uint32_t(before.hi1>>32)};
                auto values=products;
                for (unsigned n=0;n<8;++n) {
                    if (name=="pmaddh") values[n]=accumulators[n]+products[n];
                    if (name=="pmsubh") values[n]=accumulators[n]-products[n];
                    if (name=="phmadh" && !(n%2)) values[n]=products[n]+products[n+1];
                    if (name=="phmsbh") values[n]=n%2 ? ~products[n] : products[n+1]-products[n];
                    put_lane(n%4<2 ? expected_lo : expected_hi,n/4*2+n%2,32,values[n]);
                    if (!(n%2)) put_lane(expected,n/2,32,values[n]);
                }
            }
            check(execute(state)==Stop::syscall && state.executed==1);
            check(state.lo==expected_lo.lo && state.lo1==expected_lo.hi && state.hi==expected_hi.lo && state.hi1==expected_hi.hi);
            if (const auto destination=ee_batch::destinations[variant]; destination && name!="pdivbw") check_register(state.gpr[destination],expected);
            check_register(state.gpr[0],{77,88});
        }
    }
}
void scalar_and_control() {
    for (const auto rounding : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        const auto previous=std::fegetround(); std::fesetround(rounding);
        for (const auto [input,expected] : std::array<std::pair<std::uint32_t,std::uint32_t>,6>{{
            {0,0},{1,0x3f800000},{0xffffffff,0xbf800000},{0x80000000,0xcf000000},{0x7fffffff,0x4effffff},{0x01000001,0x4b800000}}}) {
            context="cvt_s_w"; State state; state.pc=ee_batch::scalar_pc(context); state.fpr[2]=input; state.fcr31=0x12345678;
            check(execute(state)==Stop::syscall && state.fpr[4]==expected && state.fcr31==0x12345678);
        }
        for (const auto [input,expected] : std::array<std::pair<std::uint32_t,std::uint32_t>,8>{{
            {1,0},{0x80000001,0},{0x3fc00000,1},{0xbfc00000,0xffffffff},{0x4f000000,0x7fffffff},
            {0xcf000000,0x80000000},{0x7fc00000,0x7fffffff},{0xffc00000,0x80000000}}}) {
            context="cvt_w_s"; State state; state.pc=ee_batch::scalar_pc(context); state.fpr[2]=input; state.fcr31=0x12345678;
            check(execute(state)==Stop::syscall && state.fpr[4]==expected && state.fcr31==0x12345678);
        }
        context="sqrt_s";
        for (const auto values : std::array<std::array<std::uint32_t,3>,9>{{
            {0,0,0},{0x80000000,0x80000000,0},{0x40800000,0x40000000,0},
            {0xc0800000,0x40000000,0x20040},{0x40000000,0x3fb504f3,0},
            {0x00800000,0x20000000,0},{0x80000001,0x80000000,0},{0x7f800000,0x5f800000,0},{0xff800000,0x5f800000,0x20040}}}) {
            State state; state.pc=ee_batch::scalar_pc(context); state.fpr[3]=values[0]; state.fcr31=0x830000;
            check(execute(state)==Stop::syscall && state.fpr[4]==values[1] && state.fcr31==(0x800000|values[2]));
        }
        context="div_s";
        for (const auto values : std::array<std::array<std::uint32_t,4>,8>{{
            {0x3f800000,0x40400000,0x3eaaaaab,0}, {0x3f800000,0,0x7fffffff,0x10020},
            {0,0,0x7fffffff,0x20040}, {0xbf800000,0,0xffffffff,0x10020}, {0x00800000,0x40000000,0,0},
            {0x7f800000,0x40000000,0x7f000000,0},{0x7fffffff,0x3f000000,0x7fffffff,0},
            {0xff800000,0x7f800000,0xbf800000,0}}}) {
            State state; state.pc=ee_batch::scalar_pc(context); state.fpr[2]=values[0]; state.fpr[3]=values[1]; state.fcr31=0x830000;
            check(execute(state)==Stop::syscall && state.fpr[4]==values[2] && state.fcr31==(0x800000|values[3]));
        }
        State failed; failed.pc=ee_batch::scalar_pc("div_s"); failed.fpr[2]=0x7f800000; failed.fpr[3]=0x3f800000; failed.fpr[4]=99; failed.fcr31=0x830000;
        check(execute(failed)==Stop::syscall && failed.fpr[4]==0x7f800000 && failed.executed==1 && failed.fcr31==0x800000);
        std::fesetround(previous);
    }
    for (const auto name : {"min_s","max_s"}) {
        context=name; State state; state.pc=ee_batch::scalar_pc(name); state.fpr[2]=0x80000000; state.fpr[3]=0; state.fcr31=0xffffffff;
        check(execute(state)==Stop::syscall && state.fpr[4]==(context=="min_s" ? 0x80000000U : 0U) && state.fcr31==0xffff3fff);
    }
    for (const auto name : {"suba_s","mula_s"}) {
        context=name; State state; state.pc=ee_batch::scalar_pc(name); state.fpr[2]=0x40400000; state.fpr[3]=0x40000000; state.fcr31=0xc000;
        check(execute(state)==Stop::syscall && state.accumulator==(context=="suba_s" ? 0x3f800000U : 0x40c00000U) && state.fcr31==0);
    }
    for (const auto name : {"madd_s","msub_s","madda_s","msuba_s"}) {
        context=name;
        const bool to_acc=context=="madda_s" || context=="msuba_s";
        for (const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            const auto saved=std::fegetround(); std::fesetround(mode);
            for (const auto acc:{0U,0x3f800000U,0xbf800000U}) {
                State state; state.pc=ee_batch::scalar_pc(name);
                state.accumulator=acc; state.fpr[2]=0x00800000; state.fpr[3]=0xbf000000; state.fpr[4]=123;
                state.fcr31=0x0083c010;
                check(execute(state)==Stop::syscall && state.executed==1 &&
                      (to_acc ? state.accumulator : state.fpr[4])==acc && state.fcr31==0x00830018);
                check(to_acc ? state.fpr[4]==123 : state.accumulator==acc);
            }
            std::fesetround(saved);
        }
        State failed; failed.pc=ee_batch::scalar_pc(name); failed.fpr[2]=0x7f800000;
        failed.fpr[3]=0x3f800000; failed.accumulator=77; failed.fpr[4]=99; failed.fcr31=0x830000;
        check(execute(failed)==Stop::syscall && failed.executed==1 &&
              (to_acc ? failed.accumulator : failed.fpr[4])==(context=="msub_s" || context=="msuba_s" ? 0xff800000U : 0x7f800000U) &&
              !failed.accumulator_overflow && failed.fcr31==0x830000);
        const bool subtract=context=="msub_s" || context=="msuba_s";
        // Product overflow dominates ACC overflow; otherwise ACC's latch wins,
        // even if another scalar instruction has cleared the current O flag.
        for (const bool product_overflow:{false,true}) {
            State state; state.pc=ee_batch::scalar_pc(name); state.accumulator=0xffffffff;
            state.accumulator_overflow=true; state.fpr[2]=0x7fffffff;
            state.fpr[3]=product_overflow ? 0x40000000 : 0x3f800000; state.fcr31=0;
            check(execute(state)==Stop::syscall &&
                  (to_acc ? state.accumulator : state.fpr[4])==
                      (product_overflow && !subtract ? 0x7fffffffU : 0xffffffffU) &&
                  state.accumulator_overflow && state.fcr31==0x8010);
        }
        // Saturated-looking data without the overflow latch can cancel normally.
        State cancellation; cancellation.pc=ee_batch::scalar_pc(name);
        cancellation.accumulator=subtract ? 0x7fffffff : 0xffffffff;
        cancellation.fpr[2]=0x7fffffff; cancellation.fpr[3]=0x3f800000;
        check(execute(cancellation)==Stop::syscall &&
              (to_acc ? cancellation.accumulator : cancellation.fpr[4])==0 && !cancellation.accumulator_overflow);
    }
    for (const auto name:{"suba_s","mula_s"}) {
        context=name; State state; state.pc=ee_batch::scalar_pc(name);
        state.fpr[2]=0xffffffff; state.fpr[3]=context=="mula_s" ? 0x40000000 : 0x7fffffff;
        check(execute(state)==Stop::syscall && state.accumulator==0xffffffff && state.accumulator_overflow && state.fcr31==0x8010);
        state.pc=ee_batch::scalar_pc("madd_s"); state.fpr[2]=0x7fffffff; state.fpr[3]=0x3f800000;
        check(execute(state)==Stop::syscall && state.fpr[4]==0xffffffff && state.accumulator_overflow);
        state.pc=ee_batch::scalar_pc(name); state.fpr[2]=0; state.fpr[3]=0;
        check(execute(state)==Stop::syscall && state.accumulator==0 && !state.accumulator_overflow && state.fcr31==0x10);
    }
    context="native memory initialization";
    for (const auto source : {0ULL,0x100000000ULL,1ULL,0xffffffffULL}) {
        State state; state.pc=ee_batch::scalar_pc("wired_write"); state.gpr[8].lo=source;
        const auto pc=state.pc;
        const bool accepted=static_cast<std::uint32_t>(source)<=2;
        check(execute(state)==(accepted ? Stop::syscall : Stop::unsupported_system));
        check(state.executed==(accepted ? 1U : 0U) && state.pc==pc+(accepted ? 4U : 0U));
    }
    {
        State state; state.pc=ee_batch::scalar_pc("wired_read"); state.gpr[9]={99,123};
        check(execute(state)==Stop::unsupported_system && !state.executed && state.gpr[9].lo==99);
    }
    for (const auto name : {"tag_read"}) {
        State state; state.pc=ee_batch::scalar_pc(name); state.gpr[9]={~0ULL,123};
        check(execute(state)==Stop::syscall && state.gpr[9].lo==0 && state.gpr[9].hi==123);
    }
    // Indexed tag access must not dereference its operand as a RAM address.
    for (const auto name : {"cache_tag","cache_index_flush","cache_instruction_flush","cache_branch_flush"}) {
        State indexed; indexed.pc=ee_batch::scalar_pc(name); indexed.gpr[4].lo=0xffffffff;
        check(execute(indexed)==Stop::syscall && indexed.executed==1);
    }
    context="mtsa"; State shift; shift.pc=ee_batch::scalar_pc(context); shift.gpr[8].lo=0xabcdef0123456789ULL;
    check(execute(shift)==Stop::syscall && shift.sa==0x23456789);
    context="eret";
    for (const unsigned status : {0U,2U,4U,6U}) {
        State state; state.pc=ee_batch::scalar_pc(context); state.cop0_status=status;
        state.cop0_epc=ee_batch::scalar_pc("mtsa")+4; state.cop0_error_epc=ee_batch::scalar_pc("cvt_s_w")+4;
        check(execute(state,1)==Stop::budget && state.executed==1 && !state.delay_slot);
        check(state.pc==(status&4 ? state.cop0_error_epc : state.cop0_epc) && state.cop0_status==(status&~(status&4 ? 4U : 2U)));
        check(execute(state)==Stop::syscall);
    }
    State missing; missing.pc=ee_batch::scalar_pc("eret"); missing.cop0_epc=0x700000;
    check(execute(missing)==Stop::unmapped_code && missing.pc==0x700000 && missing.executed==1);
    State delayed; delayed.pc=ee_batch::scalar_pc("eret"); delayed.delay_slot=true;
    check(execute(delayed)==Stop::invalid_state && delayed.executed==0 && delayed.delay_slot);
    context="conditional links";
    for (unsigned operation=0;operation<4;++operation) for (const auto value : {0ULL,1ULL,~0ULL}) for (const auto budget : {1U,8U}) {
        State state; const auto pc=ee_batch::branch_base+operation*32; state.pc=pc; state.gpr[8].lo=value; state.gpr[10].lo=99;
        const bool taken=operation%2 ? value!=~0ULL : value==~0ULL;
        auto stop=execute(state,budget);
        while (stop==Stop::budget) stop=execute(state,budget);
        check(stop==Stop::syscall && state.pc==pc+(taken ? 16 : 8) && state.gpr[31].lo==pc+8);
        check(state.gpr[10].lo==(operation>=2 && !taken ? 99 : pc+8) && state.executed==(operation>=2 && !taken ? 1 : 2));
    }
    for (const auto name : {"cache_writeback_invalidate","cache_invalidate","cache_writeback"}) {
        context=name; std::array<std::uint8_t,8> ram{1,2,3,4,5,6,7,8}; const auto before=ram;
        nt::ee::Memory memory{ram.data(),ram.size()}; State state; state.pc=ee_batch::scalar_pc(name); state.gpr[4].lo=0xa0000007;
        check(execute(state,memory)==Stop::syscall && state.executed==1 && ram==before);
        state.pc=ee_batch::scalar_pc(name); state.executed=0; state.gpr[4].lo=0x10000000;
        check(execute(state,memory)==Stop::memory_fault && state.executed==0 && memory.fault_address==0x10000000 && ram==before);
    }
}
void trap_boundaries() {
    context="integer traps";
    for (unsigned form=0;form<2;++form) for (unsigned operation=0;operation<6;++operation)
        for (const auto a : {0ULL,1ULL,0x8000000000000000ULL,~0ULL}) for (const auto b : {0ULL,1ULL,~0ULL}) {
            State state; state.pc=ee_batch::trap_base+(form*6+operation)*8;
            state.gpr[8].lo=a; state.gpr[9].lo=b; const auto before=state;
            const auto right=form ? ~0ULL : b;
            const auto x=std::bit_cast<std::int64_t>(std::uint64_t(a)),y=std::bit_cast<std::int64_t>(std::uint64_t(right));
            const bool triggered=operation==0 ? x>=y : operation==1 ? a>=right : operation==2 ? x<y :
                                 operation==3 ? a<right : operation==4 ? a==right : a!=right;
            check(execute(state)==(triggered ? Stop::trap : Stop::syscall));
            check(state.executed==(triggered ? 0 : 1) && state.pc==before.pc+(triggered ? 0 : 4));
            check_register(state.gpr[8],before.gpr[8]); check_register(state.gpr[9],before.gpr[9]);
        }
    context="trap delay-slot retry";
    State delayed; delayed.pc=ee_batch::trap_delay_pc;
    check(execute(delayed)==Stop::trap && delayed.executed==1 && delayed.delay_slot && delayed.pc==ee_batch::trap_delay_pc+4);
    delayed.gpr[9].lo=1;
    check(execute(delayed)==Stop::syscall && delayed.executed==2 && !delayed.delay_slot && delayed.pc==ee_batch::trap_delay_pc+12);
    context="prefetch"; State state; state.pc=ee_batch::prefetch_pc; state.gpr[8].lo=0x10000000;
    check(execute(state)==Stop::syscall && state.executed==1);
}
void vector_families() {
    const std::array<unsigned,4> masks{15,10,5,0},destinations{4,2,3,0};
    for (std::size_t index=0;index<ee_batch::vector.size();++index) {
        const auto name=ee_batch::vector[index].name;
        for (unsigned m=0;m<4;++m) for (unsigned d=0;d<4;++d) {
            context=std::string(name)+" mask="+std::to_string(m)+" destination="+std::to_string(d);
            State state; state.pc=ee_batch::vector_pc(index,m,d); state.gpr[0]={77,88}; state.gpr[8]={0x123456789abcdef0ULL,0xfedcba9876543210ULL};
            nt::ee::VectorStorage vectors; vectors.mac=0xabcd; vectors.status=0xabc;
            vectors.values[1]={0x3f800000,0xc0000000,0,0x40400000};
            vectors.values[2]={0x40000000,0x40800000,0xc0800000,0x3f000000};
            vectors.values[3]={11,22,33,44};
            const bool to_float=name.starts_with("vitof"),to_integer=name.starts_with("vftoi");
            const unsigned scale=name.ends_with("15") ? 15 : name.ends_with("12") ? 12 : name.ends_with('4') ? 4 : 0;
            if (to_float) vectors.values[1]={0xffff0000,32768,16777217,0};
            if (to_integer) vectors.values[1]={0xbfc00000,0x3fc00000,0x4f000000,0x80000001};
            const auto before=vectors;
            nt::ee::Memory memory; memory.vectors=&vectors;
            check(execute(state,memory)==Stop::syscall && state.executed==1);
            if (name.starts_with("qmfc")) {
                const auto destination=std::array{8U,2U,3U,0U}[d];
                if (destination) check_register(state.gpr[destination],std::bit_cast<Register>(m&1 ? Words{0,0,0,0x3f800000} : before.values[1]));
                check(vectors.values==before.values);
            } else if (name.starts_with("qmtc")) {
                const auto destination=std::array{2U,8U,3U,0U}[d];
                const auto transferred=m&1 ? Words{} : std::bit_cast<Words>(state.gpr[8]);
                for (unsigned reg=1;reg<32;++reg)
                    check(vectors.values[reg-1]==(reg==destination ? transferred : before.values[reg-1]));
            }
            else if (name!="vnop") {
                Words result{};
                if (name=="vmul") result={0x40000000,0xc1000000,0x80000000,0x3fc00000};
                if (name=="vmul_bc") result={0x3f000000,0xbf800000,0,0x3fc00000};
                if (name=="vmax") result={0x40000000,0x40800000,0,0x40400000};
                if (name=="vmini") result={0x3f800000,0xc0000000,0xc0800000,0x3f000000};
                if (name=="vmax_bc") result={0x40800000,0x40800000,0x40800000,0x40800000};
                if (name=="vmini_bc") result={0xc0800000,0xc0800000,0xc0800000,0xc0800000};
                if (name=="vabs") result={0x3f800000,0x40000000,0,0x40400000};
                if (to_float) result={0xc7800000U-(scale<<23),0x47000000U-(scale<<23),0x4b800000U-(scale<<23),0};
                if (to_integer) result={0U-(scale ? 3U<<(scale-1) : 1U),scale ? 3U<<(scale-1) : 1U,0x7fffffff,0};
                const auto destination=destinations[d];
                if (destination) {
                    auto expected=before.values[destination-1];
                    for (unsigned n=0;n<4;++n) if (masks[m]&(8U>>n)) expected[n]=result[n];
                    check(vectors.values[destination-1]==expected);
                }
                for (unsigned r=1;r<32;++r) if (r!=destination) check(vectors.values[r-1]==before.values[r-1]);
            }
            if (!name.starts_with("vmul")) check(vectors.mac==before.mac && vectors.status==before.status);
            else {
                const auto mac=(masks[m]&2 ? 2U : 0U)|((name=="vmul" ? masks[m]&6 : masks[m]&4)<<4);
                const auto current=(mac&15 ? 1U : 0U)|(mac&0xf0 ? 2U : 0U);
                check(vectors.mac==mac && vectors.status==((before.status&0xff0)|current|(current<<6)));
            }
            check_register(state.gpr[0],{77,88});
            state.pc=ee_batch::vector_pc(index,m,d); state.executed=0; vectors=before; vectors.active=true;
            check(execute(state,memory)==Stop::vector_pending && state.executed==0 && vectors.values==before.values);
            memory.vectors=nullptr;
            check(execute(state,memory)==Stop::unsupported_vector && state.executed==0);
        }
        if (name.starts_with("vmul")) {
            State state; state.pc=ee_batch::vector_pc(index); nt::ee::VectorStorage vectors;
            vectors.values[1]={0x3f800000,0x3f800000,0x3f800000,0x7f800000}; vectors.values[2].fill(0x3f800000);
            vectors.values[3]={11,22,33,44}; vectors.mac=77; vectors.status=88; const auto before=vectors;
            nt::ee::Memory memory; memory.vectors=&vectors;
            check(execute(state,memory)==Stop::syscall && state.executed==1 &&
                  vectors.values[3][3]==0x7f800000 && vectors.mac==0 && vectors.status==(before.status&0xff0));
        }
    }
}
}
void run_ee_batch_tests() {
    {
        context="native vector rounding, masks, aliases and exceptional ranges";
        const auto previous_rounding=std::fegetround();
        std::uint32_t seed=0x715ac329;
        const auto random=[&] { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
        for (const auto rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            std::fesetround(rounding);
            for (unsigned i=0;i<30000;++i) for (unsigned operation=0;operation<5;++operation) {
                nt::ee::VectorStorage vectors{};
                const auto sample=[&] {
                    const auto raw=random();
                    return i%2 ? raw : (raw&0x807fffffU)|((100U+raw%50)<<23);
                };
                for (unsigned lane=0;lane<4;++lane) {
                    vectors.values[0][lane]=sample(); vectors.values[1][lane]=sample();
                    vectors.accumulator[lane]=sample();
                }
                if (i%17==0) vectors.values[0]={0,0x80000000,1,0x80000001};
                if (i%19==0) vectors.values[1]=vectors.values[0];
                const auto mask=i%16,broadcast=(i/16)%5,destination=i%3 ? 1U : 32U;
                vectors.status=random()&0xfff; vectors.mac=random()&0xffff;
                const auto before=vectors;
                const auto expected=nt::numeric::binary_vector(destination==32 ? before.accumulator : before.values[0],
                    before.values[0],before.values[1],mask,broadcast,before.status,[&](auto a,auto b,unsigned lane) {
                        if (operation<2) return nt::numeric::add_subtract_finite(a,b,operation==1,true);
                        if (operation==2) return nt::numeric::multiply_finite(a,b,true);
                        return nt::numeric::vector_multiply_accumulate(before.accumulator[lane],a,b,operation==4);
                    });
                nt::ee::Memory memory; memory.vectors=&vectors;
                const auto stop=operation<2 ? nt_ee_vector_add(&memory,destination,1,2,mask,operation==1,broadcast) :
                    operation==2 ? nt_ee_vector_multiply(&memory,destination,1,2,mask,broadcast) :
                    nt_ee_vector_multiply_accumulate(&memory,destination,1,2,mask,operation==4,broadcast);
                check(stop==unsigned(expected.supported ? Stop::budget : Stop::unsupported_numeric));
                if (expected.supported) {
                    check((destination==32 ? vectors.accumulator : vectors.values[0])==expected.value);
                    check(vectors.mac==expected.mac && vectors.status==expected.status);
                } else {
                    check(vectors.values==before.values && vectors.accumulator==before.accumulator);
                    check(vectors.mac==before.mac && vectors.status==before.status);
                }
                check(std::fegetround()==rounding);
            }
        }
        std::fesetround(previous_rounding);
    }
    packed_arithmetic();
    integer_products_and_funnels();
    accumulator_packing();
    halfword_products();
    scalar_and_control();
    vector_families();
    trap_boundaries();
}
