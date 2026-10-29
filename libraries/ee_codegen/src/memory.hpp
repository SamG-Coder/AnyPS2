#pragma once
#include "llvm_writer.hpp"
#include <optional>

namespace nt::ee_codegen {
struct MemoryOperation { unsigned bytes; bool write,sign; };
inline std::optional<MemoryOperation> memory_operation(Op op) {
    switch (op) {
    case Op::lb: return MemoryOperation{1,false,true};
    case Op::lbu: return MemoryOperation{1,false,false};
    case Op::lh: return MemoryOperation{2,false,true};
    case Op::lhu: return MemoryOperation{2,false,false};
    case Op::lw: return MemoryOperation{4,false,true};
    case Op::lwu: return MemoryOperation{4,false,false};
    case Op::ld: return MemoryOperation{8,false,false};
    case Op::lq: return MemoryOperation{16,false,false};
    case Op::sb: return MemoryOperation{1,true,false};
    case Op::sh: return MemoryOperation{2,true,false};
    case Op::sw: return MemoryOperation{4,true,false};
    case Op::sd: return MemoryOperation{8,true,false};
    case Op::sq: return MemoryOperation{16,true,false};
    default: return {};
    }
}
inline std::uint32_t physical_address(std::uint32_t address) {
    return address>=0x80000000U && address<0xc0000000U ? address&0x1fffffffU : address;
}
inline std::string code_overlap(Writer& w,const std::string& offset,const std::string& end,std::span<const CodeRange> code) {
    std::string overlap="false";
    for (const auto& range:code) {
        const auto begin=range.address;
        const auto finish=std::uint64_t(begin)+range.size;
        const auto before=w.value("icmp ult i64 "+offset+", "+std::to_string(finish));
        const auto after=w.value("icmp ugt i64 "+end+", "+std::to_string(begin));
        overlap=w.binary("or","i1",overlap,w.binary("and","i1",before,after));
    }
    return overlap;
}
inline void emit_memory(Writer& w,const r5900::Instruction& i,MemoryOperation access,
                        std::span<const CodeRange> code) {
    const auto suffix=std::to_string(i.pc);
    const auto address=w.binary("add","i32",w.narrow(w.reg(i.rs)),std::to_string(i.immediate));
    const auto segment=w.binary("and","i32",address,"3221225472");
    const auto alias=w.value("icmp eq i32 "+segment+", 2147483648");
    const auto masked=w.binary("and","i32",address,"536870911");
    auto physical=w.value("select i1 "+alias+", i32 "+masked+", i32 "+address);
    if (access.bytes==16) physical=w.binary("and","i32",physical,"4294967280");
    const auto offset=w.value("zext i32 "+physical+" to i64");
    const auto end=w.binary("add","i64",offset,std::to_string(access.bytes));
    const auto size=w.load("i64",offsetof(ee::Memory,size),"%memory");
    const auto data=w.load("ptr",offsetof(ee::Memory,data),"%memory");
    const auto null=w.value("icmp eq ptr "+data+", null");
    const auto outside=w.value("icmp ugt i64 "+end+", "+size);
    const auto device=w.value("icmp ugt i64 "+end+", 33554432");
    const auto low=w.binary("and","i32",address,std::to_string(access.bytes==16 ? 0 : access.bytes-1));
    const auto unaligned=w.value("icmp ne i32 "+low+", 0");
    const auto invalid=w.binary("or","i1",w.binary("or","i1",outside,device),w.binary("or","i1",null,unaligned));
    w.out<<"  br i1 "<<invalid<<", label %fault"<<suffix<<", label %valid"<<suffix<<"\nfault"<<suffix<<":\n";
    w.store("i32",address,offsetof(ee::Memory,fault_address),"%memory");
    w.out<<"  ret i32 4\nvalid"<<suffix<<":\n";
    if (access.write) {
        const auto overlap=code_overlap(w,offset,end,code);
        w.out<<"  br i1 "<<overlap<<", label %code_write"<<suffix<<", label %access"<<suffix<<"\ncode_write"<<suffix<<":\n";
        w.store("i32",address,offsetof(ee::Memory,fault_address),"%memory");
        w.out<<"  ret i32 5\naccess"<<suffix<<":\n";
    }
    const auto pointer=w.value("getelementptr i8, ptr "+data+", i64 "+offset);
    const auto type=access.bytes==16 ? std::string("<4 x i32>") : "i"+std::to_string(access.bytes*8);
    if (access.write) {
        auto value=access.bytes==16 ? w.vector_reg(i.rt) : w.reg(i.rt);
        if (access.bytes<8) value=w.value("trunc i64 "+value+" to "+type);
        w.out<<"  store "<<type<<" "<<value<<", ptr "<<pointer<<", align 1\n";
    } else {
        auto value=w.value("load "+type+", ptr "+pointer+", align 1");
        if (access.bytes<8) value=w.value(std::string(access.sign ? "sext " : "zext ")+type+" "+value+" to i64");
        if (i.rt) w.store(access.bytes==16 ? type : "i64",value,offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
    }
}
}
