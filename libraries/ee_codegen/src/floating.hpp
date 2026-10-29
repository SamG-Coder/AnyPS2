#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline bool floating_operation(Op op) {
    switch (op) {
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
    case Op::sqrt_s: case Op::div_s: {
        const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
        const auto divided=w.value(i.op==Op::sqrt_s ? "call i64 @nt_ee_square_root_finite(i32 "+right+")" :
                                   "call i64 @nt_ee_divide_finite(i32 "+left+", i32 "+right+")");
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
    case Op::suba_s: case Op::mula_s: case Op::adda_s: case Op::add_s: case Op::sub_s: case Op::mul_s: {
        const auto left=w.load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
        const auto right=w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
        const auto sum=w.value(i.op==Op::mul_s || i.op==Op::mula_s ? "call i64 @nt_ee_multiply_finite(i32 "+left+", i32 "+right+")" : "call i64 @nt_ee_add_finite(i32 "+left+", i32 "+right+", i32 "+(i.op==Op::sub_s || i.op==Op::suba_s ? "1" : "0")+")");
        const auto unsupported=w.value("icmp uge i64 "+sum+", 8589934592");
        const auto suffix=std::to_string(i.pc);
        w.out<<"  br i1 "<<unsupported<<", label %numeric_stop"<<suffix<<", label %numeric_result"<<suffix
           <<"\nnumeric_stop"<<suffix<<":\n  ret i32 6\nnumeric_result"<<suffix<<":\n";
        const auto underflow=w.value("icmp uge i64 "+sum+", 4294967296");
        const auto flags=w.load("i32",offsetof(runtime::State,fcr31));
        const auto cleared=w.binary("and","i32",flags,"4294918143");
        const auto added=w.value("select i1 "+underflow+", i32 16392, i32 0");
        w.store("i32",w.narrow(sum),(i.op==Op::adda_s || i.op==Op::suba_s || i.op==Op::mula_s) ? offsetof(runtime::State,accumulator) :
              offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
        w.store("i32",w.binary("or","i32",cleared,added),offsetof(runtime::State,fcr31));
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
            const auto invalid=w.binary("or","i1",w.value("icmp eq i32 "+le+", 2139095040"),w.value("icmp eq i32 "+re+", 2139095040"));
            const auto suffix=std::to_string(i.pc);
            w.out<<"  br i1 "<<invalid<<", label %compare_stop"<<suffix<<", label %compare_values"<<suffix
               <<"\ncompare_stop"<<suffix<<":\n  ret i32 6\ncompare_values"<<suffix<<":\n";
            const auto normalized=[&](const std::string& bits,const std::string& exponent) {
                const auto zero=w.value("icmp eq i32 "+exponent+", 0");
                const auto finite=w.value("select i1 "+zero+", i32 0, i32 "+bits);
                return w.value("bitcast i32 "+finite+" to float");
            };
            const auto a=normalized(left,le),b=normalized(right,re);
            condition=w.value(std::string("fcmp ")+(predicate==2 ? "oeq" : predicate==4 ? "olt" : "ole")+" float "+a+", "+b);
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
