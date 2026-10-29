#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline const char* trap_predicate(Op op) {
    switch (op) {
    case Op::tge: case Op::tgei: return "sge";
    case Op::tgeu: case Op::tgeiu: return "uge";
    case Op::tlt: case Op::tlti: return "slt";
    case Op::tltu: case Op::tltiu: return "ult";
    case Op::teq: case Op::teqi: return "eq";
    case Op::tne: case Op::tnei: return "ne";
    default: return nullptr;
    }
}
inline void emit_trap(Writer& w,const r5900::Instruction& i,const char* predicate) {
    const bool immediate=i.op==Op::tgei || i.op==Op::tgeiu || i.op==Op::tlti || i.op==Op::tltiu || i.op==Op::teqi || i.op==Op::tnei;
    const auto right=immediate ? std::to_string(std::int64_t(i.immediate)) : w.reg(i.rt);
    const auto condition=w.value(std::string("icmp ")+predicate+" i64 "+w.reg(i.rs)+", "+right);
    const auto suffix=std::to_string(i.pc);
    w.out<<"  br i1 "<<condition<<", label %trap_stop"<<suffix<<", label %trap_continue"<<suffix
         <<"\ntrap_stop"<<suffix<<":\n  ret i32 "<<static_cast<unsigned>(ee::Stop::trap)
         <<"\ntrap_continue"<<suffix<<":\n";
}
}
