#pragma once
#include "llvm_writer.hpp"
#include <optional>

namespace nt::ee_codegen {
enum class PackedKind { add, sub, greater, maximum, minimum, absolute, signed_add, signed_sub,
    unsigned_add, unsigned_sub, mixed_half, variable_left, variable_right, variable_arithmetic,
    shift_left, shift_right, shift_arithmetic, permutation, funnel };
struct PackedOperation { PackedKind kind; unsigned width; };
inline std::optional<PackedOperation> packed_operation(Op op) {
    switch (op) {
    case Op::paddw: return PackedOperation{PackedKind::add,32};
    case Op::psubw: return PackedOperation{PackedKind::sub,32};
    case Op::pcgtw: return PackedOperation{PackedKind::greater,32};
    case Op::pmaxw: return PackedOperation{PackedKind::maximum,32};
    case Op::pminw: return PackedOperation{PackedKind::minimum,32};
    case Op::pabsw: return PackedOperation{PackedKind::absolute,32};
    case Op::paddsw: return PackedOperation{PackedKind::signed_add,32};
    case Op::psubsw: return PackedOperation{PackedKind::signed_sub,32};
    case Op::psubuw: return PackedOperation{PackedKind::unsigned_sub,32};
    case Op::paddh: return PackedOperation{PackedKind::add,16};
    case Op::psubh: return PackedOperation{PackedKind::sub,16};
    case Op::pcgth: return PackedOperation{PackedKind::greater,16};
    case Op::pmaxh: return PackedOperation{PackedKind::maximum,16};
    case Op::pminh: return PackedOperation{PackedKind::minimum,16};
    case Op::pabsh: return PackedOperation{PackedKind::absolute,16};
    case Op::paddsh: return PackedOperation{PackedKind::signed_add,16};
    case Op::psubsh: return PackedOperation{PackedKind::signed_sub,16};
    case Op::padduh: return PackedOperation{PackedKind::unsigned_add,16};
    case Op::psubuh: return PackedOperation{PackedKind::unsigned_sub,16};
    case Op::paddb: return PackedOperation{PackedKind::add,8};
    case Op::psubb: return PackedOperation{PackedKind::sub,8};
    case Op::pcgtb: return PackedOperation{PackedKind::greater,8};
    case Op::paddsb: return PackedOperation{PackedKind::signed_add,8};
    case Op::psubsb: return PackedOperation{PackedKind::signed_sub,8};
    case Op::paddub: return PackedOperation{PackedKind::unsigned_add,8};
    case Op::psubub: return PackedOperation{PackedKind::unsigned_sub,8};
    case Op::padsbh: return PackedOperation{PackedKind::mixed_half,16};
    case Op::psllvw: return PackedOperation{PackedKind::variable_left,32};
    case Op::psrlvw: return PackedOperation{PackedKind::variable_right,32};
    case Op::psravw: return PackedOperation{PackedKind::variable_arithmetic,32};
    case Op::psllh: return PackedOperation{PackedKind::shift_left,16};
    case Op::psrlh: return PackedOperation{PackedKind::shift_right,16};
    case Op::psrah: return PackedOperation{PackedKind::shift_arithmetic,16};
    case Op::psllw: return PackedOperation{PackedKind::shift_left,32};
    case Op::psrlw: return PackedOperation{PackedKind::shift_right,32};
    case Op::psraw: return PackedOperation{PackedKind::shift_arithmetic,32};
    case Op::qfsrv: return PackedOperation{PackedKind::funnel,32};
    case Op::pexeh: return PackedOperation{PackedKind::permutation,16};
    case Op::prevh: return PackedOperation{PackedKind::permutation,16};
    case Op::pexew: return PackedOperation{PackedKind::permutation,32};
    case Op::prot3w: return PackedOperation{PackedKind::permutation,32};
    case Op::pexch: return PackedOperation{PackedKind::permutation,16};
    case Op::pexcw: return PackedOperation{PackedKind::permutation,32};
    case Op::pinth: return PackedOperation{PackedKind::permutation,16};
    case Op::pinteh: return PackedOperation{PackedKind::permutation,16};
    default: return {};
    }
}
inline std::string vector_constant(unsigned lanes,unsigned width,std::int64_t value) {
    std::string result="<";
    for (unsigned lane=0;lane<lanes;++lane) {
        if (lane) result+=", ";
        result+="i"+std::to_string(width)+" "+std::to_string(value);
    }
    return result+">";
}
inline void emit_packed(Writer& w,const r5900::Instruction& i,PackedOperation operation) {
    using Kind=PackedKind;
    const auto kind=operation.kind;
    const auto width=operation.width;
    const auto lanes=128/width;
    const auto type="<"+std::to_string(lanes)+" x i"+std::to_string(width)+">";
    const auto predicate="<"+std::to_string(lanes)+" x i1>";
    const auto operand=[&](unsigned index) {
        const auto raw=w.vector_reg(index);
        return width==32 ? raw : w.value("bitcast <4 x i32> "+raw+" to "+type);
    };
    const auto left=operand(i.rs),right=operand(i.rt);
    std::string result;
    switch (kind) {
    case Kind::permutation: {
        std::string mask;
        switch (i.op) {
        case Op::pexeh: mask="<i32 2, i32 1, i32 0, i32 3, i32 6, i32 5, i32 4, i32 7>"; break;
        case Op::prevh: mask="<i32 3, i32 2, i32 1, i32 0, i32 7, i32 6, i32 5, i32 4>"; break;
        case Op::pexew: mask="<i32 2, i32 1, i32 0, i32 3>"; break;
        case Op::prot3w: mask="<i32 1, i32 2, i32 0, i32 3>"; break;
        case Op::pexch: mask="<i32 0, i32 2, i32 1, i32 3, i32 4, i32 6, i32 5, i32 7>"; break;
        case Op::pexcw: mask="<i32 0, i32 2, i32 1, i32 3>"; break;
        case Op::pinth: mask="<i32 0, i32 12, i32 1, i32 13, i32 2, i32 14, i32 3, i32 15>"; break;
        case Op::pinteh: mask="<i32 0, i32 8, i32 2, i32 10, i32 4, i32 12, i32 6, i32 14>"; break;
        default: throw std::logic_error("Invalid packed permutation");
        }
        result=w.value("shufflevector "+type+" "+right+", "+type+" "+left+", <"+std::to_string(lanes)+" x i32> "+mask); break;
    }
    case Kind::funnel: {
        const auto amount=w.load("i64",offsetof(runtime::State,sa));
        // MTSA can hold an arbitrary word. Only the documented 0..15-byte
        // funnel domain is supported; never emit an out-of-range LLVM shift.
        const auto invalid=w.value("icmp ugt i64 "+amount+", 15");
        const auto suffix=std::to_string(i.pc);
        w.out<<"  br i1 "<<invalid<<", label %funnel_stop"<<suffix<<", label %funnel_shift"<<suffix
             <<"\nfunnel_stop"<<suffix<<":\n  ret i32 10\nfunnel_shift"<<suffix<<":\n";
        const auto a=w.value("bitcast <4 x i32> "+left+" to i128");
        const auto b=w.value("bitcast <4 x i32> "+right+" to i128");
        const auto shift=w.value("zext i64 "+w.binary("shl","i64",amount,"3")+" to i128");
        const auto joined=w.value("call i128 @llvm.fshr.i128(i128 "+a+", i128 "+b+", i128 "+shift+")");
        result=w.value("bitcast i128 "+joined+" to <4 x i32>"); break;
    }

    case Kind::add: case Kind::sub:
        result=w.binary(kind==Kind::add ? "add" : "sub",type,left,right); break;
    case Kind::greater:
        result=w.value("sext "+predicate+" "+w.value("icmp sgt "+type+" "+left+", "+right)+" to "+type); break;
    case Kind::maximum: case Kind::minimum: {
        const auto choose=w.value(std::string("icmp ")+(kind==Kind::maximum ? "sgt" : "slt")+" "+type+" "+left+", "+right);
        result=w.value("select "+predicate+" "+choose+", "+type+" "+left+", "+type+" "+right); break;
    }
    case Kind::absolute: {
        const auto negative=w.value("icmp slt "+type+" "+right+", zeroinitializer");
        const auto negated=w.binary("sub",type,"zeroinitializer",right);
        const auto magnitude=w.value("select "+predicate+" "+negative+", "+type+" "+negated+", "+type+" "+right);
        const auto minimum=w.value("icmp eq "+type+" "+right+", "+vector_constant(lanes,width,-(1LL<<(width-1))));
        result=w.value("select "+predicate+" "+minimum+", "+type+" "+vector_constant(lanes,width,(1LL<<(width-1))-1)+", "+type+" "+magnitude); break;
    }
    case Kind::signed_add: case Kind::signed_sub: case Kind::unsigned_add: case Kind::unsigned_sub: {
        const auto intrinsic=kind==Kind::signed_add ? "sadd" : kind==Kind::signed_sub ? "ssub" : kind==Kind::unsigned_add ? "uadd" : "usub";
        result=w.value(std::string("call ")+type+" @llvm."+intrinsic+".sat.v"+std::to_string(lanes)+"i"+std::to_string(width)+"("+type+" "+left+", "+type+" "+right+")"); break;
    }
    case Kind::mixed_half: {
        const auto sum=w.binary("add",type,left,right),difference=w.binary("sub",type,left,right);
        result=w.value("select <8 x i1> <i1 false, i1 false, i1 false, i1 false, i1 true, i1 true, i1 true, i1 true>, "+type+" "+sum+", "+type+" "+difference); break;
    }
    case Kind::shift_left: case Kind::shift_right: case Kind::shift_arithmetic:
        result=w.binary(kind==Kind::shift_left ? "shl" : kind==Kind::shift_right ? "lshr" : "ashr",type,right,vector_constant(lanes,width,i.sa&(width-1))); break;
    case Kind::variable_left: case Kind::variable_right: case Kind::variable_arithmetic: {
        const auto amounts=w.value("shufflevector <4 x i32> "+left+", <4 x i32> poison, <2 x i32> <i32 0, i32 2>");
        const auto values=w.value("shufflevector <4 x i32> "+right+", <4 x i32> poison, <2 x i32> <i32 0, i32 2>");
        const auto masked=w.binary("and","<2 x i32>",amounts,"<i32 31, i32 31>");
        const auto shifted=w.binary(kind==Kind::variable_left ? "shl" : kind==Kind::variable_right ? "lshr" : "ashr","<2 x i32>",values,masked);
        const auto extended=w.value("sext <2 x i32> "+shifted+" to <2 x i64>");
        result=w.value("bitcast <2 x i64> "+extended+" to <4 x i32>"); break;
    }
    }
    if (i.rd) w.store(type,result,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
}
inline void declare_packed_intrinsics(std::ostream& output) {
    output<<"declare i128 @llvm.fshr.i128(i128, i128, i128)\n";
    for (const unsigned width : {8U,16U,32U}) {
        const auto lanes=std::to_string(128/width),bits=std::to_string(width);
        const auto type="<"+lanes+" x i"+bits+">";
        for (const auto operation : {"sadd","ssub","uadd","usub"})
            output<<"declare "<<type<<" @llvm."<<operation<<".sat.v"<<lanes<<"i"<<bits<<"("<<type<<", "<<type<<")\n";
    }
}
}
