#pragma once
#include "integer_multiply.hpp"

namespace nt::ee_codegen {
inline bool halfword_product(Op op) {
    return op==Op::pmulth || op==Op::pmaddh || op==Op::pmsubh || op==Op::phmadh || op==Op::phmsbh || op==Op::pdivbw;
}
inline void emit_halfword_product(Writer& w,const r5900::Instruction& i) {
    const auto left=w.vector_reg(i.rs),right=w.vector_reg(i.rt);
    if (i.op==Op::pdivbw) {
        const auto halves=w.value("bitcast <4 x i32> "+right+" to <8 x i16>");
        const auto divisor=w.value("sext i16 "+w.value("extractelement <8 x i16> "+halves+", i32 0")+" to i64");
        std::string quotients="poison",remainders="poison";
        for (unsigned lane=0;lane<4;++lane) {
            const auto number=std::to_string(lane);
            const auto dividend=w.extend(w.value("extractelement <4 x i32> "+left+", i32 "+number));
            const auto result=divide_words(w,dividend,divisor,true);
            quotients=w.value("insertelement <4 x i32> "+quotients+", i32 "+w.narrow(result.quotient)+", i32 "+number);
            remainders=w.value("insertelement <4 x i32> "+remainders+", i32 "+w.narrow(result.remainder)+", i32 "+number);
        }
        write_accumulator(w,quotients,false); write_accumulator(w,remainders,true);
        return;
    }
    const auto widen=[&](const std::string& source) {
        const auto halves=w.value("bitcast <4 x i32> "+source+" to <8 x i16>");
        return w.value("sext <8 x i16> "+halves+" to <8 x i32>");
    };
    auto product=w.binary("mul","<8 x i32>",widen(left),widen(right));
    if (i.op==Op::pmaddh || i.op==Op::pmsubh) {
        const auto low=read_accumulator(w,false),high=read_accumulator(w,true);
        const auto accumulators=w.value("shufflevector <4 x i32> "+low+", <4 x i32> "+high+", <8 x i32> <i32 0, i32 1, i32 4, i32 5, i32 2, i32 3, i32 6, i32 7>");
        product=w.binary(i.op==Op::pmaddh ? "add" : "sub","<8 x i32>",accumulators,product);
    } else if (i.op==Op::phmadh || i.op==Op::phmsbh) {
        const auto paired=w.value("shufflevector <8 x i32> "+product+", <8 x i32> poison, <8 x i32> <i32 1, i32 0, i32 3, i32 2, i32 5, i32 4, i32 7, i32 6>");
        const auto combined=w.binary(i.op==Op::phmadh ? "add" : "sub","<8 x i32>",paired,product);
        if (i.op==Op::phmsbh)
            product=w.binary("xor","<8 x i32>",product,"<i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1, i32 -1>");
        product=w.value("select <8 x i1> <i1 true, i1 false, i1 true, i1 false, i1 true, i1 false, i1 true, i1 false>, <8 x i32> "+combined+", <8 x i32> "+product);
    }
    const auto low=w.value("shufflevector <8 x i32> "+product+", <8 x i32> poison, <4 x i32> <i32 0, i32 1, i32 4, i32 5>");
    const auto high=w.value("shufflevector <8 x i32> "+product+", <8 x i32> poison, <4 x i32> <i32 2, i32 3, i32 6, i32 7>");
    write_accumulator(w,low,false); write_accumulator(w,high,true);
    if (i.rd) {
        const auto selected=w.value("shufflevector <8 x i32> "+product+", <8 x i32> poison, <4 x i32> <i32 0, i32 2, i32 4, i32 6>");
        w.store("<4 x i32>",selected,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
    }
}
}
