#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline std::string branch_condition(Writer& w,const r5900::Instruction& i) {
    const char* predicate=nullptr;
    bool compare_registers=false;
    switch (i.op) {
    case Op::bc1f: case Op::bc1t: case Op::bc1fl: case Op::bc1tl: {
        const auto condition=w.binary("and","i32",w.load("i32",offsetof(runtime::State,fcr31)),"8388608");
        const bool positive=i.op==Op::bc1t || i.op==Op::bc1tl;
        return w.value(std::string(positive ? "icmp ne i32 " : "icmp eq i32 ")+condition+", 0");
    }
    case Op::beq: case Op::beql: predicate="eq"; compare_registers=true; break;
    case Op::bne: case Op::bnel: predicate="ne"; compare_registers=true; break;
    case Op::blez: case Op::blezl: predicate="sle"; break;
    case Op::bgtz: case Op::bgtzl: predicate="sgt"; break;
    case Op::bltz: case Op::bltzl: predicate="slt"; break;
    case Op::bgez: case Op::bgezl: predicate="sge"; break;
    default: return "true";
    }
    return w.value(std::string("icmp ")+predicate+" i64 "+w.reg(i.rs)+", "+
                   (compare_registers ? w.reg(i.rt) : "0"));
}
}
