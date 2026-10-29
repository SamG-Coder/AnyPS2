#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline void emit_performance_transfer(Writer& w,const r5900::Instruction& i) {
    const bool write=i.op==Op::mtc0;
    const auto selector=i.word&63U;
    // Public MTPS encodings with nonzero register field have no effect.
    if (write && !(selector&1) && (selector&62)) return;
    const auto base=offsetof(ee::Memory,performance);
    const auto control=w.load("i32",base+offsetof(ee::PerformanceState,control),"%memory");
    const auto active=w.value("icmp uge i32 "+control+", 2147483648");
    std::string source;
    auto unsupported=active;
    if (write) {
        source=w.narrow(w.reg(i.rt));
        if (!(selector&1)) unsupported=w.binary("or","i1",active,w.value("icmp uge i32 "+source+", 2147483648"));
    }
    const auto suffix=std::to_string(i.pc);
    w.out<<"  br i1 "<<unsupported<<", label %system_stop"<<suffix<<", label %system_ready"<<suffix
         <<"\nsystem_stop"<<suffix<<":\n  ret i32 10\nsystem_ready"<<suffix<<":\n";
    const auto offset=base+((selector&1) ? offsetof(ee::PerformanceState,counters)+((selector>>1)&1)*sizeof(std::uint32_t) : offsetof(ee::PerformanceState,control));
    if (write) w.store("i32",source,offset,"%memory");
    else if (i.rt) w.store("i64",w.extend(w.load("i32",offset,"%memory")),offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
}
inline void emit_system_transfer(Writer& w,const r5900::Instruction& i) {
    if (i.rd==25) { emit_performance_transfer(w,i); return; }
    const auto offset=i.rd==12 ? offsetof(runtime::State,cop0_status) :
                      i.rd==14 ? offsetof(runtime::State,cop0_epc) : offsetof(runtime::State,cop0_error_epc);
    if (i.op==Op::mtc0) w.store("i32",w.narrow(w.reg(i.rt)),offset);
    else {
        auto value=w.load("i32",offset);
        if (i.rd==12) value=w.binary("and","i32",value,std::to_string(0xf0c79c1fU));
        if (i.rt) w.store("i64",w.extend(value),offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
    }
}
}
