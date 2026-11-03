#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline bool floating_operation(Op op) {
    switch (op) {
    case Op::rsqrt_s: case Op::madd_s: case Op::msub_s: case Op::madda_s: case Op::msuba_s:
    case Op::cvt_s_w: case Op::cvt_w_s: case Op::min_s: case Op::max_s:
    case Op::div_s: case Op::sqrt_s: case Op::suba_s: case Op::mula_s:
    case Op::adda_s: case Op::add_s: case Op::sub_s: case Op::mul_s:
    case Op::mov_s: case Op::abs_s: case Op::neg_s: case Op::compare_s: return true;
    default: return false;
    }
}
inline void emit_floating(Writer& w,const r5900::Instruction& i) {
    switch (i.op) {
    case Op::cvt_s_w: case Op::cvt_w_s: case Op::min_s: case Op::max_s: {
        const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        std::string converted;
        if (i.op==Op::cvt_s_w || i.op==Op::cvt_w_s)
            converted=w.value(std::string("call i32 @nt_ee_")+(i.op==Op::cvt_s_w ? "integer_to_scalar" : "scalar_to_integer")+"(i32 "+left+")");
        else {
            const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
            converted=w.value("call i32 @nt_ee_min_max_scalar(i32 "+left+", i32 "+right+", i32 "+(i.op==Op::max_s ? "1" : "0")+")");
        }
        w.store("i32",converted,offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
        if (i.op==Op::min_s || i.op==Op::max_s) {
            const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
            w.store("i32",w.binary("and","i32",flags,"4294918143"),offsetof(runtime::State,fcr31));
        }

        return;
    }
    case Op::rsqrt_s: case Op::sqrt_s: case Op::div_s: {
        const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
        const auto divided=w.value(i.op==Op::sqrt_s ? "call i64 @nt_ee_square_root_finite(i32 "+right+")" :
                                   (i.op==Op::rsqrt_s ? "call i64 @nt_ee_reciprocal_square_root_finite(i32 " : "call i64 @nt_ee_divide_finite(i32 ")+left+", i32 "+right+")");
        const auto unsupported=w.value("icmp slt i64 "+divided+", 0");
        const auto suffix=std::to_string(i.pc);
        w.out<<"  br i1 "<<unsupported<<", label %divide_stop"<<suffix<<", label %divide_result"<<suffix
           <<"\ndivide_stop"<<suffix<<":\n  ret i32 6\ndivide_result"<<suffix<<":\n";
        const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
        const auto added=w.narrow(w.binary("lshr","i64",divided,"32"));
        w.store("i32",w.narrow(divided),offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
        w.store("i32",w.binary("or","i32",w.binary("and","i32",flags,"4294770687"),added),offsetof(runtime::State,fcr31));
        return;
    }
    case Op::madd_s: case Op::msub_s: case Op::madda_s: case Op::msuba_s:
    case Op::suba_s: case Op::mula_s: case Op::adda_s: case Op::add_s: case Op::sub_s: case Op::mul_s: {
        const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
        const bool accumulate=i.op==Op::madd_s || i.op==Op::msub_s || i.op==Op::madda_s || i.op==Op::msuba_s;
        const auto sum=accumulate ? w.value("call i64 @nt_ee_scalar_multiply_accumulate(i32 "+
            w.load("i32",offsetof(runtime::State,accumulator))+", i32 "+left+", i32 "+right+", i32 "+
            (i.op==Op::msub_s || i.op==Op::msuba_s ? "1" : "0")+", i32 "+
            w.value("zext i8 "+w.load("i8",offsetof(runtime::State,accumulator_overflow))+" to i32")+")") : w.value(i.op==Op::mul_s || i.op==Op::mula_s ? "call i64 @nt_ee_scalar_multiply(i32 "+left+", i32 "+right+")" : "call i64 @nt_ee_scalar_add(i32 "+left+", i32 "+right+", i32 "+(i.op==Op::sub_s || i.op==Op::suba_s ? "1" : "0")+")");
        const auto unsupported=w.value("icmp ne i64 "+w.binary("and","i64",sum,"8589934592")+", 0");
        const auto suffix=std::to_string(i.pc);
        w.out<<"  br i1 "<<unsupported<<", label %numeric_stop"<<suffix<<", label %numeric_result"<<suffix
           <<"\nnumeric_stop"<<suffix<<":\n  ret i32 6\nnumeric_result"<<suffix<<":\n";
        const auto underflow=w.value("icmp ne i64 "+w.binary("and","i64",sum,"4294967296")+", 0");
        const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
        const auto cleared=w.binary("and","i32",flags,"4294918143");
        const auto added=w.value("select i1 "+underflow+", i32 16392, i32 0");
        const auto sticky=w.value("icmp ne i64 "+w.binary("and","i64",sum,"17179869184")+", 0");
        const auto sticky_flag=w.value("select i1 "+sticky+", i32 8, i32 0");
        const auto overflow=w.value("icmp ne i64 "+w.binary("and","i64",sum,"34359738368")+", 0");
        const auto overflow_flag=w.value("select i1 "+overflow+", i32 32784, i32 0");
        const bool to_acc=i.op==Op::adda_s || i.op==Op::suba_s || i.op==Op::mula_s || i.op==Op::madda_s || i.op==Op::msuba_s;
        if (to_acc) w.store("i8",w.value("zext i1 "+overflow+" to i8"),offsetof(runtime::State,accumulator_overflow));
        w.store("i32",w.narrow(sum),(i.op==Op::adda_s || i.op==Op::suba_s || i.op==Op::mula_s || i.op==Op::madda_s || i.op==Op::msuba_s) ? offsetof(runtime::State,accumulator) :
              offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
        w.store("i32",w.binary("or","i32",w.binary("or","i32",w.binary("or","i32",cleared,added),sticky_flag),overflow_flag),offsetof(runtime::State,fcr31));
        return;
    }
    case Op::mov_s: case Op::abs_s: case Op::neg_s: {
        auto bits=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        if (i.op==Op::abs_s) bits=w.binary("and","i32",bits,"2147483647");
        if (i.op==Op::neg_s) bits=w.binary("xor","i32",bits,"2147483648");
        w.store("i32",bits,offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
        if (i.op!=Op::mov_s) {
            const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
            w.store("i32",w.binary("and","i32",flags,"4294918143"),offsetof(runtime::State,fcr31));
        }
        return;
    }
    case Op::compare_s: {
        const auto predicate=i.word&15U;
        std::string condition="false";
        if (predicate) {
            const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
            const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
            const auto le=w.binary("and","i32",left,"2139095040");
            const auto re=w.binary("and","i32",right,"2139095040");
            // Raw signed-magnitude ordering includes exponent 255; no IEEE NaNs.
            // Both signs of zero and denormal inputs compare as the same zero.
            const auto key=[&](const std::string& bits,const std::string& exponent) {
                const auto normalized=w.value("select i1 "+w.value("icmp eq i32 "+exponent+", 0")+", i32 0, i32 "+bits);
                const auto negative=w.value("icmp slt i32 "+normalized+", 0");
                return w.value("select i1 "+negative+", i32 "+w.binary("xor","i32",normalized,"4294967295")+
                               ", i32 "+w.binary("or","i32",normalized,"2147483648"));
            };
            const auto a=key(left,le),b=key(right,re);
            condition=w.value(std::string("icmp ")+(predicate==2 ? "eq" : predicate==4 ? "ult" : "ule")+" i32 "+a+", "+b);
        }
        const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
        const auto kept=w.binary("and","i32",flags,"4286578687");
        const auto changed=w.value("select i1 "+condition+", i32 8388608, i32 0");
        w.store("i32",w.binary("or","i32",kept,changed),offsetof(runtime::State,fcr31)); return;
    }
    default: throw std::logic_error("Invalid floating operation");
    }
}
}
