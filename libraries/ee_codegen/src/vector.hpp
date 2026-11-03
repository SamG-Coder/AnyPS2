#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline bool vector_operation(Op op) {
    switch (op) {
    case Op::vcallms: case Op::vcallmsr: return true;
    case Op::vdiv: case Op::vsqrt: case Op::vrsqrt: case Op::vwaitq: case Op::vclip:
    case Op::vopmula: case Op::vopmsub: case Op::vrnext: case Op::vrget: case Op::vrinit: case Op::vrxor: return true;
    case Op::vadd_q: case Op::vsub_q: case Op::vadd_i: case Op::vsub_i:
    case Op::vadd: case Op::vsub: case Op::vadd_bc: case Op::vsub_bc:
    case Op::vadda: case Op::vsuba: case Op::vadda_bc: case Op::vsuba_bc: case Op::vmul_q: case Op::vmul_i:
    case Op::vmadd: case Op::vmsub: case Op::vmadd_bc: case Op::vmsub_bc:
    case Op::vmadda: case Op::vmsuba: case Op::vmadda_bc: case Op::vmsuba_bc: case Op::vmula:
    case Op::vmula_bc: case Op::vmul: case Op::vmul_bc: case Op::vmax: case Op::vmax_bc: case Op::vmini: case Op::vmini_bc:
    case Op::vmove: case Op::vmr32: case Op::vabs: case Op::vnop:
    case Op::vitof0: case Op::vitof4: case Op::vitof12: case Op::vitof15:
    case Op::vftoi0: case Op::vftoi4: case Op::vftoi12: case Op::vftoi15: return true;
    default: return false;
    }
}
inline void emit_vector(Writer& w,const r5900::Instruction& i) {
    const auto suffix=std::to_string(i.pc);
    if (i.op==Op::vdiv || i.op==Op::vsqrt || i.op==Op::vrsqrt || i.op==Op::vopmula || i.op==Op::vopmsub) {
        const bool outer=i.op==Op::vopmula || i.op==Op::vopmsub;
        const auto arguments=outer ? "ptr %memory, i32 "+std::to_string(i.op==Op::vopmula ? 32 : i.sa)+
            ", i32 "+std::to_string(i.rd)+", i32 "+std::to_string(i.rt)+", i32 "+(i.op==Op::vopmsub ? "1" : "0") :
            "ptr %memory, i32 "+std::to_string(i.rd)+", i32 "+std::to_string(i.rt)+", i32 "+std::to_string(i.rs&3)+
            ", i32 "+std::to_string((i.rs>>2)&3)+", i32 "+std::to_string(i.op==Op::vdiv ? 0 : i.op==Op::vsqrt ? 1 : 2);
        const auto result=w.value(std::string("call i32 @nt_ee_vector_")+(outer ? "outer" : "quotient")+"("+arguments+")");
        const auto ready=w.value("icmp eq i32 "+result+", 0");
        w.out<<"  br i1 "<<ready<<", label %special_ready"<<suffix<<", label %special_stop"<<suffix
             <<"\nspecial_stop"<<suffix<<":\n  ret i32 "<<result<<"\nspecial_ready"<<suffix<<":\n";
        return;
    }
    if (i.op==Op::vcallms || i.op==Op::vcallmsr) {
        const auto result=w.value("call i32 @nt_ee_vector_launch(ptr %memory, i32 "+
            std::string(i.op==Op::vcallmsr ? "1" : "0")+", i32 "+std::to_string((i.word>>6)&0x7fffU)+")");
        const auto ready=w.value("icmp eq i32 "+result+", 0");
        w.out<<"  br i1 "<<ready<<", label %launch_ready"<<suffix<<", label %launch_stop"<<suffix
             <<"\nlaunch_stop"<<suffix<<":\n  ret i32 "<<result<<"\nlaunch_ready"<<suffix<<":\n";
        return;
    }
    const bool accumulate=i.op==Op::vmadd || i.op==Op::vmsub || i.op==Op::vmadd_bc || i.op==Op::vmsub_bc ||
        i.op==Op::vmadda || i.op==Op::vmsuba || i.op==Op::vmadda_bc || i.op==Op::vmsuba_bc;
    const bool accumulator=i.op==Op::vmula || i.op==Op::vmula_bc || i.op==Op::vmadda || i.op==Op::vmsuba ||
        i.op==Op::vmadda_bc || i.op==Op::vmsuba_bc || i.op==Op::vadda || i.op==Op::vsuba || i.op==Op::vadda_bc || i.op==Op::vsuba_bc;
    const bool multiply=i.op==Op::vmul || i.op==Op::vmul_bc || i.op==Op::vmula || i.op==Op::vmula_bc || i.op==Op::vmul_q || i.op==Op::vmul_i;
    const bool scalar_q=i.op==Op::vmul_q || i.op==Op::vadd_q || i.op==Op::vsub_q;
    const bool scalar_i=i.op==Op::vmul_i || i.op==Op::vadd_i || i.op==Op::vsub_i;
    const bool add_subtract=i.op==Op::vadd_q || i.op==Op::vsub_q || i.op==Op::vadd_i || i.op==Op::vsub_i || i.op==Op::vadd || i.op==Op::vsub || i.op==Op::vadd_bc || i.op==Op::vsub_bc ||
        i.op==Op::vadda || i.op==Op::vsuba || i.op==Op::vadda_bc || i.op==Op::vsuba_bc;
    if (multiply || add_subtract || accumulate) {
        const bool subtract=i.op==Op::vsub_q || i.op==Op::vsub_i || i.op==Op::vsub || i.op==Op::vsub_bc || i.op==Op::vmsub || i.op==Op::vmsub_bc ||
            i.op==Op::vmsuba || i.op==Op::vmsuba_bc || i.op==Op::vsuba || i.op==Op::vsuba_bc;
        const bool broadcast=i.op==Op::vadd_bc || i.op==Op::vsub_bc || i.op==Op::vmul_bc || i.op==Op::vmula_bc ||
            i.op==Op::vmadd_bc || i.op==Op::vmsub_bc || i.op==Op::vmadda_bc || i.op==Op::vmsuba_bc || i.op==Op::vadda_bc || i.op==Op::vsuba_bc;
        auto arguments="ptr %memory, i32 "+std::to_string(accumulator ? 32 : i.sa)+", i32 "+std::to_string(i.rd)+
            ", i32 "+std::to_string(scalar_q ? 32 : scalar_i ? 33 : i.rt)+", i32 "+std::to_string(i.rs&15);
        if (!multiply) arguments+=std::string(", i32 ")+(subtract ? "1" : "0");
        arguments+=", i32 "+std::to_string(broadcast ? i.word&3U : 4U);
        const auto outcome=w.value(std::string("call i32 @nt_ee_vector_")+
            (accumulate ? "multiply_accumulate" : multiply ? "multiply" : "add")+"("+arguments+")");
        const auto ready=w.value("icmp eq i32 "+outcome+", 0");
        w.out<<"  br i1 "<<ready<<", label %vector_math_ready"<<suffix<<", label %vector_math_stop"<<suffix
             <<"\nvector_math_stop"<<suffix<<":\n  ret i32 "<<outcome<<"\nvector_math_ready"<<suffix<<":\n";
        return;
    }
    const auto vectors=w.vector_storage(suffix);
    if (i.op==Op::vnop || i.op==Op::vwaitq) return;
    if (i.op==Op::vclip) {
        const auto left=w.vu_reg(vectors,i.rd),right=w.vu_reg(vectors,i.rt);
        std::string args;
        for (unsigned lane=0;lane<4;++lane) {
            if (lane) args+=", ";
            args+="i32 "+w.value("extractelement <4 x i32> "+(lane==3 ? right : left)+", i32 "+std::to_string(lane));
        }
        const auto flags=w.value("call i32 @nt_vu_clip_flags("+args+")");
        const auto history=w.binary("shl","i32",w.load("i32",offsetof(ee::VectorStorage,clip),vectors),"6");
        const auto value=w.binary("and","i32",w.binary("or","i32",history,flags),"16777215");
        w.store("i32",value,offsetof(ee::VectorStorage,clip),vectors);
        return;
    }
    if (i.op==Op::vrinit || i.op==Op::vrxor || i.op==Op::vrnext || i.op==Op::vrget) {
        auto value=w.load("i32",offsetof(ee::VectorStorage,random),vectors);
        if (i.op==Op::vrinit || i.op==Op::vrxor) {
            const auto source=w.value("extractelement <4 x i32> "+w.vu_reg(vectors,i.rd)+", i32 "+std::to_string(i.rs&3));
            value=i.op==Op::vrinit ? source : w.binary("xor","i32",value,source);
        } else {
            if (!i.rt) return;
            if (i.op==Op::vrnext) {
                const auto feedback=w.binary("and","i32",w.binary("xor","i32",w.binary("lshr","i32",value,"4"),
                    w.binary("lshr","i32",value,"22")),"1");
                value=w.binary("or","i32",w.binary("shl","i32",value,"1"),feedback);
            }
        }
        value=w.binary("or","i32",w.binary("and","i32",value,"8388607"),"1065353216");
        if (i.op!=Op::vrget) w.store("i32",value,offsetof(ee::VectorStorage,random),vectors);
        if (i.op==Op::vrnext || i.op==Op::vrget) {
            auto result=w.vu_reg(vectors,i.rt);
            for (unsigned lane=0;lane<4;++lane) if (i.rs&(8U>>lane))
                result=w.value("insertelement <4 x i32> "+result+", i32 "+value+", i32 "+std::to_string(lane));
            w.store("<4 x i32>",result,offsetof(ee::VectorStorage,values)+(i.rt-1)*16,vectors);
        }
        return;
    }
    const bool min_max=i.op==Op::vmax || i.op==Op::vmax_bc || i.op==Op::vmini || i.op==Op::vmini_bc;
    const unsigned destination=min_max ? i.sa : i.rt;
    // These operations do not update flags. Ownership is still observed, but
    // ignored writes and empty masks need no arithmetic or source loads.
    if (!destination || !(i.rs&15)) return;
    auto source=w.vu_reg(vectors,i.rd);
    if (i.op==Op::vmr32)
        source=w.value("shufflevector <4 x i32> "+source+", <4 x i32> poison, <4 x i32> <i32 1, i32 2, i32 3, i32 0>");
    else if (i.op==Op::vabs)
        source=w.binary("and","<4 x i32>",source,"<i32 2147483647, i32 2147483647, i32 2147483647, i32 2147483647>");
    else if (min_max) {
        auto right=w.vu_reg(vectors,i.rt);
        if (i.op==Op::vmax_bc || i.op==Op::vmini_bc) {
            const auto lane=std::to_string(i.word&3U);
            right=w.value("shufflevector <4 x i32> "+right+", <4 x i32> poison, <4 x i32> <i32 "+lane+", i32 "+lane+", i32 "+lane+", i32 "+lane+">");
        }
        const auto key=[&](const std::string& bits) {
            const auto signs=w.binary("ashr","<4 x i32>",bits,"<i32 31, i32 31, i32 31, i32 31>");
            const auto mask=w.binary("or","<4 x i32>",signs,"<i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>");
            return w.binary("xor","<4 x i32>",bits,mask);
        };
        const auto a=key(source),b=key(right);
        const auto choose=w.value(std::string("icmp ")+(i.op==Op::vmax || i.op==Op::vmax_bc ? "ugt" : "ult")+" <4 x i32> "+a+", "+b);
        source=w.value("select <4 x i1> "+choose+", <4 x i32> "+source+", <4 x i32> "+right);
    } else if (i.op!=Op::vmove && i.op!=Op::vmr32 && i.op!=Op::vabs) {
        const bool to_float=i.op==Op::vitof0 || i.op==Op::vitof4 || i.op==Op::vitof12 || i.op==Op::vitof15;
        const auto scale=i.op==Op::vitof4 || i.op==Op::vftoi4 ? 4 : i.op==Op::vitof12 || i.op==Op::vftoi12 ? 12 : i.op==Op::vitof15 || i.op==Op::vftoi15 ? 15 : 0;
        auto converted=source;
        for (unsigned lane=0;lane<4;++lane) {
            if (!(i.rs&(8U>>lane))) continue;
            const auto scalar=w.value("extractelement <4 x i32> "+source+", i32 "+std::to_string(lane));
            const auto result=w.value(std::string("call i32 @nt_ee_")+(to_float ? "integer_to_scaled_scalar" : "scaled_scalar_to_integer")+"(i32 "+scalar+", i32 "+std::to_string(scale)+")");
            converted=w.value("insertelement <4 x i32> "+converted+", i32 "+result+", i32 "+std::to_string(lane));
        }
        source=converted;
    }
    std::string mask="<";
    for (unsigned lane=0;lane<4;++lane) {
        if (lane) mask+=", ";
        mask+=std::string("i1 ")+((i.rs&(8U>>lane)) ? "true" : "false");
    }
    mask+=">";
    const auto merged=w.value("select <4 x i1> "+mask+", <4 x i32> "+source+", <4 x i32> "+w.vu_reg(vectors,destination));
    if (destination) w.store("<4 x i32>",merged,offsetof(ee::VectorStorage,values)+(destination-1)*sizeof(std::array<std::uint32_t,4>),vectors);
}
}
