#pragma once
#include "llvm_writer.hpp"
#include <optional>

namespace nt::ee_codegen {
struct IntegerMultiply { bool signed_operation, divide, accumulate, packed, secondary; };
inline std::optional<IntegerMultiply> integer_multiply(Op op) {
    switch (op) {
    case Op::mult: return IntegerMultiply{true,false,false,false,false};
    case Op::multu: return IntegerMultiply{false,false,false,false,false};
    case Op::div: return IntegerMultiply{true,true,false,false,false};
    case Op::divu: return IntegerMultiply{false,true,false,false,false};
    case Op::mult1: return IntegerMultiply{true,false,false,false,true};
    case Op::multu1: return IntegerMultiply{false,false,false,false,true};
    case Op::div1: return IntegerMultiply{true,true,false,false,true};
    case Op::divu1: return IntegerMultiply{false,true,false,false,true};
    case Op::madd: return IntegerMultiply{true,false,true,false,false};
    case Op::maddu: return IntegerMultiply{false,false,true,false,false};
    case Op::madd1: return IntegerMultiply{true,false,true,false,true};
    case Op::maddu1: return IntegerMultiply{false,false,true,false,true};
    case Op::pmultw: return IntegerMultiply{true,false,false,true,false};
    case Op::pmultuw: return IntegerMultiply{false,false,false,true,false};
    case Op::pdivw: return IntegerMultiply{true,true,false,true,false};
    case Op::pdivuw: return IntegerMultiply{false,true,false,true,false};
    case Op::pmadduw: return IntegerMultiply{false,false,true,true,false};
    default: return {};
    }
}
struct WordDivision { std::string quotient,remainder; };
inline WordDivision divide_words(Writer& w,const std::string& a,const std::string& b,bool signed_operation) {
    const auto zero=w.value("icmp eq i64 "+b+", 0");
    const auto denominator=w.value("select i1 "+zero+", i64 1, i64 "+b);
    const auto divided=w.binary(signed_operation ? "sdiv" : "udiv","i64",a,denominator);
    const auto modulus=w.binary(signed_operation ? "srem" : "urem","i64",a,denominator);
    const auto fallback=signed_operation ? w.value("select i1 "+w.value("icmp slt i64 "+a+", 0")+", i64 1, i64 -1") : "-1";
    return {w.value("select i1 "+zero+", i64 "+fallback+", i64 "+divided),
            w.value("select i1 "+zero+", i64 "+a+", i64 "+modulus)};
}
inline void emit_integer_multiply(Writer& w,const r5900::Instruction& i,IntegerMultiply operation) {
    // Capture both source vectors before any aliased destination write.
    const auto left=w.vector_reg(i.rs),right=w.vector_reg(i.rt);
    std::array<std::string,2> results;
    const unsigned count=operation.packed ? 2 : 1;
    for (unsigned lane=0;lane<count;++lane) {
        const bool secondary=operation.secondary || lane!=0;
        const auto lo_offset=secondary ? offsetof(runtime::State,lo1) : offsetof(runtime::State,lo);
        const auto hi_offset=secondary ? offsetof(runtime::State,hi1) : offsetof(runtime::State,hi);
        const auto operand=[&](const std::string& vector) {
            const auto low=w.value("extractelement <4 x i32> "+vector+", i32 "+std::to_string(lane*2));
            return operation.signed_operation ? w.extend(low) : w.value("zext i32 "+low+" to i64");
        };
        const auto a=operand(left),b=operand(right);
        std::string quotient,remainder;
        if (!operation.divide) {
            quotient=w.binary("mul","i64",a,b);
            if (operation.accumulate) {
                const auto old_lo=w.binary("and","i64",w.load("i64",lo_offset),"4294967295");
                const auto old_hi=w.binary("shl","i64",w.load("i64",hi_offset),"32");
                quotient=w.binary("add","i64",quotient,w.binary("or","i64",old_lo,old_hi));
            }
            remainder=w.binary("lshr","i64",quotient,"32");
        } else {
            // Widen first, including INT32_MIN / -1. Select a nonzero divisor
            // before emitting LLVM division so exceptional cases never poison IR.
            const auto result=divide_words(w,a,b,operation.signed_operation);
            quotient=result.quotient; remainder=result.remainder;
        }
        const auto lo=w.extend(w.narrow(quotient)),hi=w.extend(w.narrow(remainder));
        w.store("i64",lo,lo_offset); w.store("i64",hi,hi_offset);
        results[lane]=operation.packed ? quotient : lo;
    }
    if (!operation.divide && i.rd) {
        w.store("i64",results[0],offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
        if (operation.packed) w.store("i64",results[1],offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register)+offsetof(runtime::Register,hi));
    }
}
inline std::string read_accumulator(Writer& w,bool high) {
    const auto first=w.load("i64",high ? offsetof(runtime::State,hi) : offsetof(runtime::State,lo));
    const auto second=w.load("i64",high ? offsetof(runtime::State,hi1) : offsetof(runtime::State,lo1));
    const auto low=w.value("insertelement <2 x i64> poison, i64 "+first+", i32 0");
    const auto whole=w.value("insertelement <2 x i64> "+low+", i64 "+second+", i32 1");
    return w.value("bitcast <2 x i64> "+whole+" to <4 x i32>");
}
inline void write_accumulator(Writer& w,const std::string& words,bool high) {
    const auto value=w.value("bitcast <4 x i32> "+words+" to <2 x i64>");
    w.store("i64",w.value("extractelement <2 x i64> "+value+", i32 0"),high ? offsetof(runtime::State,hi) : offsetof(runtime::State,lo));
    w.store("i64",w.value("extractelement <2 x i64> "+value+", i32 1"),high ? offsetof(runtime::State,hi1) : offsetof(runtime::State,lo1));
}
inline void emit_accumulator_pack(Writer& w,const r5900::Instruction& i) {
    const auto low=read_accumulator(w,false),high=read_accumulator(w,true);
    if (i.op==Op::pmthl) {
        const auto source=w.vector_reg(i.rs);
        const auto next_low=w.value("shufflevector <4 x i32> "+low+", <4 x i32> "+source+", <4 x i32> <i32 4, i32 1, i32 6, i32 3>");
        const auto next_high=w.value("shufflevector <4 x i32> "+high+", <4 x i32> "+source+", <4 x i32> <i32 5, i32 1, i32 7, i32 3>");
        write_accumulator(w,next_low,false); write_accumulator(w,next_high,true);
        return;
    }
    if (!i.rd) return;
    std::string result,type="<4 x i32>";
    if (i.sa==0 || i.sa==1) {
        const auto order=i.sa==0 ? "<i32 0, i32 4, i32 2, i32 6>" : "<i32 1, i32 5, i32 3, i32 7>";
        result=w.value("shufflevector <4 x i32> "+low+", <4 x i32> "+high+", <4 x i32> "+order);
    } else if (i.sa==2) {
        const auto words=w.value("shufflevector <4 x i32> "+low+", <4 x i32> "+high+", <4 x i32> <i32 0, i32 4, i32 2, i32 6>");
        const auto wide=w.value("bitcast <4 x i32> "+words+" to <2 x i64>");
        const auto above=w.value("icmp sgt <2 x i64> "+wide+", <i64 2147483647, i64 2147483647>");
        const auto upper=w.value("select <2 x i1> "+above+", <2 x i64> <i64 2147483647, i64 2147483647>, <2 x i64> "+wide);
        const auto below=w.value("icmp slt <2 x i64> "+upper+", <i64 -2147483648, i64 -2147483648>");
        result=w.value("select <2 x i1> "+below+", <2 x i64> <i64 -2147483648, i64 -2147483648>, <2 x i64> "+upper);
        type="<2 x i64>";
    } else {
        auto words=w.value("shufflevector <4 x i32> "+low+", <4 x i32> "+high+", <8 x i32> <i32 0, i32 1, i32 4, i32 5, i32 2, i32 3, i32 6, i32 7>");
        if (i.sa==4) {
            const auto minimum="<i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768>";
            const auto maximum="<i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767>";
            const auto above=w.value("icmp sgt <8 x i32> "+words+", "+maximum);
            words=w.value("select <8 x i1> "+above+", <8 x i32> "+maximum+", <8 x i32> "+words);
            const auto below=w.value("icmp slt <8 x i32> "+words+", "+minimum);
            words=w.value("select <8 x i1> "+below+", <8 x i32> "+minimum+", <8 x i32> "+words);
        }
        result=w.value("trunc <8 x i32> "+words+" to <8 x i16>"); type="<8 x i16>";
    }
    w.store(type,result,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
}
inline bool packed_accumulator_transfer(Op op) {
    return op==Op::pmfhl || op==Op::pmthl || op==Op::pmfhi || op==Op::pmflo || op==Op::pmthi || op==Op::pmtlo;
}
inline void emit_packed_accumulator_transfer(Writer& w,const r5900::Instruction& i) {
    if (i.op==Op::pmfhl || i.op==Op::pmthl) { emit_accumulator_pack(w,i); return; }
    const bool high=i.op==Op::pmfhi || i.op==Op::pmthi;
    const bool write=i.op==Op::pmthi || i.op==Op::pmtlo;
    const auto first=high ? offsetof(runtime::State,hi) : offsetof(runtime::State,lo);
    const auto second=high ? offsetof(runtime::State,hi1) : offsetof(runtime::State,lo1);
    if (write) {
        const auto source=w.value("bitcast <4 x i32> "+w.vector_reg(i.rs)+" to <2 x i64>");
        w.store("i64",w.value("extractelement <2 x i64> "+source+", i32 0"),first);
        w.store("i64",w.value("extractelement <2 x i64> "+source+", i32 1"),second);
    } else if (i.rd) {
        w.store("i64",w.load("i64",first),offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
        w.store("i64",w.load("i64",second),offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register)+offsetof(runtime::Register,hi));
    }
}
}
