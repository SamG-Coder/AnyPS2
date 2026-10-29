#pragma once
#include "llvm_writer.hpp"

namespace nt::ee_codegen {
inline bool address_space_operation(Op op) {
    return op==Op::tlbr || op==Op::tlbwi || op==Op::tlbwr || op==Op::tlbp;
}
inline void emit_address_space(Writer& w,const r5900::Instruction& i) {
    using ee::AddressSpaceOperation;
    const auto operation=i.op==Op::mfc0 ? AddressSpaceOperation::read_parameter :
        i.op==Op::mtc0 ? AddressSpaceOperation::write_parameter :
        i.op==Op::tlbr ? AddressSpaceOperation::read_mapping :
        i.op==Op::tlbwi ? AddressSpaceOperation::install_indexed :
        i.op==Op::tlbwr ? AddressSpaceOperation::install_replacement : AddressSpaceOperation::probe;
    const auto parameter=(i.op==Op::mfc0 || i.op==Op::mtc0) ? i.rd : 0U;
    const auto source=i.op==Op::mtc0 ? w.narrow(w.reg(i.rt)) : std::string("0");
    const auto packed=w.value("call i64 @nt_ee_address_space(ptr %memory, i32 "+
        std::to_string(static_cast<unsigned>(operation))+", i32 "+std::to_string(parameter)+", i32 "+source+")");
    const auto status=w.narrow(w.binary("lshr","i64",packed,"32"));
    const auto ready=w.value("icmp eq i32 "+status+", 0");
    const auto suffix=std::to_string(i.pc);
    w.out<<"  br i1 "<<ready<<", label %mapping_ready"<<suffix<<", label %mapping_stop"<<suffix
         <<"\nmapping_stop"<<suffix<<":\n  ret i32 "<<status<<"\nmapping_ready"<<suffix<<":\n";
    if (i.op==Op::mfc0 && i.rt) w.store("i64",w.extend(w.narrow(packed)),offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
}
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
    if (i.rd==0 || i.rd==2 || i.rd==3 || i.rd==5 || i.rd==10) { emit_address_space(w,i); return; }
    // Native RAM has no reserved guest TLB entries or private cache lines.
    // Reject attempts to establish a reservation rather than silently losing it.
    if (i.rd==6 || i.rd==28) {
        if (i.op==Op::mtc0) {
            const auto nonzero=w.value("icmp ne i32 "+w.narrow(w.reg(i.rt))+", 0");
            const auto suffix=std::to_string(i.pc);
            w.out<<"  br i1 "<<nonzero<<", label %wired_stop"<<suffix<<", label %wired_ready"<<suffix
                 <<"\nwired_stop"<<suffix<<":\n  ret i32 10\nwired_ready"<<suffix<<":\n";
        } else if (i.rt) {
            w.store("i64","0",offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
        }
        return;
    }
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
