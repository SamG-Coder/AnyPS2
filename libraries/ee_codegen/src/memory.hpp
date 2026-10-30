#pragma once
#include "llvm_writer.hpp"
#include <optional>

namespace nt::ee_codegen {
enum class RegisterBank { integer, floating, vector };
enum class Merge { none, left, right };
struct MemoryOperation { unsigned bytes; bool write,sign; RegisterBank bank=RegisterBank::integer; Merge merge=Merge::none; };
inline std::optional<MemoryOperation> memory_operation(Op op) {
    switch (op) {
    case Op::cache: return MemoryOperation{1,false,false};
    case Op::lb: return MemoryOperation{1,false,true};
    case Op::lbu: return MemoryOperation{1,false,false};
    case Op::lh: return MemoryOperation{2,false,true};
    case Op::lhu: return MemoryOperation{2,false,false};
    case Op::lqc2: return MemoryOperation{16,false,false,RegisterBank::vector};
    case Op::sqc2: return MemoryOperation{16,true,false,RegisterBank::vector};
    case Op::lwc1: return MemoryOperation{4,false,false,RegisterBank::floating};
    case Op::swc1: return MemoryOperation{4,true,false,RegisterBank::floating};
    case Op::lw: return MemoryOperation{4,false,true};
    case Op::lwu: return MemoryOperation{4,false,false};
    case Op::lwl: return MemoryOperation{4,false,true,RegisterBank::integer,Merge::left};
    case Op::lwr: return MemoryOperation{4,false,true,RegisterBank::integer,Merge::right};
    case Op::swl: return MemoryOperation{4,true,false,RegisterBank::integer,Merge::left};
    case Op::swr: return MemoryOperation{4,true,false,RegisterBank::integer,Merge::right};
    case Op::sdl: return MemoryOperation{8,true,false,RegisterBank::integer,Merge::left};
    case Op::sdr: return MemoryOperation{8,true,false,RegisterBank::integer,Merge::right};
    case Op::ldl: return MemoryOperation{8,false,false,RegisterBank::integer,Merge::left};
    case Op::ldr: return MemoryOperation{8,false,false,RegisterBank::integer,Merge::right};
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
    return runtime::default_ram_address(address);
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
inline std::string merge_word(Writer& w,const std::string& source,const std::string& original,
                              const std::string& address,MemoryOperation access) {
    const auto type="i"+std::to_string(access.bytes*8),last=std::to_string(access.bytes-1);
    auto lane=w.binary("and","i32",address,last);
    if (access.merge==Merge::left) lane=w.binary("sub","i32",last,lane);
    auto shift=w.binary("mul","i32",lane,"8");
    if (access.bytes==8) shift=w.value("zext i32 "+shift+" to i64");
    const auto direction=(access.merge==Merge::left)!=access.write ? "shl" : "lshr";
    const auto changed=w.binary(direction,type,source,shift);
    const auto mask=w.binary("xor",type,w.binary(direction,type,"-1",shift),"-1");
    return w.binary("or",type,changed,w.binary("and",type,original,mask));
}
inline void emit_memory(Writer& w,const r5900::Instruction& i,MemoryOperation access,
                        std::span<const CodeRange> code) {
    const auto suffix=std::to_string(i.pc);
    // DXLTG addresses a cache index, not RAM. Native coherent storage has no
    // private valid/dirty lines; TagLo reads the empty tag in system.hpp.
    // Indexed invalidation and branch-target flush only order native work.
    // All writes to converted code remain forbidden, including after a flush.
    if (i.op==Op::cache && (i.rt==0x07 || i.rt==0x0c || i.rt==0x10 || i.rt==0x14)) {
        w.out<<"  fence seq_cst\n";
        return;
    }
    std::string vectors;
    if (access.bank==RegisterBank::vector) {
        vectors=w.vector_storage(suffix);
    }
    const auto address=w.binary("add","i32",w.narrow(w.reg(i.rs)),std::to_string(i.immediate));
    const auto segment=w.binary("and","i32",address,"3221225472");
    const auto alias=w.value("icmp eq i32 "+segment+", 2147483648");
    const auto masked=w.binary("and","i32",address,"536870911");
    auto physical=w.value("select i1 "+alias+", i32 "+masked+", i32 "+address);
    const auto in_range=[&](std::uint32_t begin,std::uint32_t end) {
        return w.binary("and","i1",w.value("icmp uge i32 "+address+", "+std::to_string(begin)),
            w.value("icmp ult i32 "+address+", "+std::to_string(end)));
    };
    const auto user_alias=w.binary("or","i1",in_range(runtime::uncached_ram_begin,runtime::uncached_ram_end),
        in_range(runtime::accelerated_ram_begin,runtime::accelerated_ram_end));
    physical=w.value("select i1 "+user_alias+", i32 "+w.binary("and","i32",address,"268435455")+", i32 "+physical);
    const auto scratchpad=in_range(ee::scratchpad_begin,ee::scratchpad_begin+ee::scratchpad_size);
    const auto main_memory=w.value("xor i1 "+scratchpad+", true");
    const bool native_io=(access.bytes==4 || access.bytes==8) && access.bank==RegisterBank::integer && access.merge==Merge::none;
    if (native_io) {
        const auto device_address=w.value("icmp uge i32 "+physical+", 268435456");
        const auto aligned=w.value("icmp eq i32 "+w.binary("and","i32",address,std::to_string(access.bytes-1))+", 0");
        const auto native_address=w.binary("and","i1",w.binary("and","i1",device_address,main_memory),aligned);
        w.out<<"  br i1 "<<native_address<<", label %io"<<suffix<<", label %ram"<<suffix
             <<"\nio"<<suffix<<":\n";
        std::string status;
        if (access.bytes==8) {
            status=access.write ? w.value("call i32 @nt_ee_io_write64(ptr %memory, i32 "+physical+", i64 "+w.reg(i.rt)+")") :
                w.value("call i32 @nt_ee_io_read64(ptr %state, ptr %memory, i32 "+physical+", i32 "+std::to_string(i.rt)+")");
        } else if (access.write) status=w.value("call i32 @nt_ee_io_write32(ptr %memory, i32 "+physical+", i32 "+w.narrow(w.reg(i.rt))+")");
        else {
            const auto packed=w.value("call i64 @nt_ee_io_read32(ptr %memory, i32 "+physical+")");
            status=w.narrow(w.binary("lshr","i64",packed,"32"));
            const auto ready=w.value("icmp eq i32 "+status+", 0");
            w.out<<"  br i1 "<<ready<<", label %io_read"<<suffix<<", label %io_stop"<<suffix<<"\nio_read"<<suffix<<":\n";
            if (i.rt) {
                const auto word=w.narrow(packed);
                w.store("i64",access.sign ? w.extend(word) : w.value("zext i32 "+word+" to i64"),offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
            }
            w.out<<"  br label %memory_done"<<suffix<<"\nio_stop"<<suffix<<":\n  ret i32 "<<status<<"\nram"<<suffix<<":\n";
        }
        if (access.write || access.bytes==8) {
            const auto ready=w.value("icmp eq i32 "+status+", 0");
            w.out<<"  br i1 "<<ready<<", label %memory_done"<<suffix<<", label %io_stop"<<suffix
                 <<"\nio_stop"<<suffix<<":\n  ret i32 "<<status<<"\nram"<<suffix<<":\n";
        }
    }
    if (access.bytes==16 && access.bank!=RegisterBank::vector) physical=w.binary("and","i32",physical,"4294967280");
    if (access.merge!=Merge::none) physical=w.binary("and","i32",physical,std::to_string(~std::uint32_t(access.bytes-1)));
    physical=w.value("select i1 "+scratchpad+", i32 "+w.binary("and","i32",physical,std::to_string(ee::scratchpad_size-1))+", i32 "+physical);
    const auto offset=w.value("zext i32 "+physical+" to i64");
    const auto end=w.binary("add","i64",offset,std::to_string(access.bytes));
    const auto size=w.value("select i1 "+scratchpad+", i64 "+w.load("i64",offsetof(ee::Memory,scratchpad_bytes),"%memory")+", i64 "+w.load("i64",offsetof(ee::Memory,size),"%memory"));
    const auto data=w.value("select i1 "+scratchpad+", ptr "+w.load("ptr",offsetof(ee::Memory,scratchpad),"%memory")+", ptr "+w.load("ptr",offsetof(ee::Memory,data),"%memory"));
    const auto null=w.value("icmp eq ptr "+data+", null");
    const auto outside=w.value("icmp ugt i64 "+end+", "+size);
    const auto limit=w.value("select i1 "+scratchpad+", i64 "+std::to_string(ee::scratchpad_size)+", i64 33554432");
    const auto device=w.value("icmp ugt i64 "+end+", "+limit);
    const auto low=w.binary("and","i32",address,std::to_string((access.bytes==16 && access.bank!=RegisterBank::vector) || access.merge!=Merge::none ? 0 : access.bytes-1));
    const auto unaligned=w.value("icmp ne i32 "+low+", 0");
    const auto invalid=w.binary("or","i1",w.binary("or","i1",outside,device),w.binary("or","i1",null,unaligned));
    w.out<<"  br i1 "<<invalid<<", label %fault"<<suffix<<", label %valid"<<suffix<<"\nfault"<<suffix<<":\n";
    w.store("i32",address,offsetof(ee::Memory,fault_address),"%memory");
    w.out<<"  ret i32 4\nvalid"<<suffix<<":\n";
    // Native RAM is coherent. These data-cache hit operations become an ordering
    // boundary; executable writes are still rejected at every memory store.
    if (i.op==Op::cache) { w.out<<"  fence seq_cst\n"; return; }
    if (access.write) {
        const auto overlap=w.binary("and","i1",main_memory,code_overlap(w,offset,end,code));
        w.out<<"  br i1 "<<overlap<<", label %code_write"<<suffix<<", label %access"<<suffix<<"\ncode_write"<<suffix<<":\n";
        w.store("i32",address,offsetof(ee::Memory,fault_address),"%memory");
        w.out<<"  ret i32 5\naccess"<<suffix<<":\n";
    }
    const auto pointer=w.value("getelementptr i8, ptr "+data+", i64 "+offset);
    const auto type=access.bytes==16 ? std::string("<4 x i32>") : "i"+std::to_string(access.bytes*8);
    if (access.bank==RegisterBank::vector) {
        const auto register_offset=offsetof(ee::VectorStorage,values)+(i.rt ? i.rt-1 : 0)*sizeof(std::array<std::uint32_t,4>);
        if (access.write) {
            const auto value=w.vu_reg(vectors,i.rt);
            w.out<<"  store "<<type<<" "<<value<<", ptr "<<pointer<<", align 1\n";
        } else {
            const auto value=w.value("load "+type+", ptr "+pointer+", align 1");
            if (i.rt) w.store(type,value,register_offset,vectors);
        }
        return;
    }
    if (access.write) {
        auto value=access.bank==RegisterBank::floating ? w.load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t)) :
            access.bytes==16 ? w.vector_reg(i.rt) : w.reg(i.rt);
        if (access.bytes<8 && access.bank!=RegisterBank::floating) value=w.value("trunc i64 "+value+" to "+type);
        if (access.merge!=Merge::none) value=merge_word(w,value,w.value("load "+type+", ptr "+pointer+", align 1"),address,access);
        w.out<<"  store "<<type<<" "<<value<<", ptr "<<pointer<<", align 1\n";
    } else {
        auto value=w.value("load "+type+", ptr "+pointer+", align 1");
        if (access.merge!=Merge::none) value=merge_word(w,value,access.bytes==4 ? w.narrow(w.reg(i.rt)) : w.reg(i.rt),address,access);
        if (access.bank==RegisterBank::floating) {
            w.store("i32",value,offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t)); return;
        }
        if (i.op==Op::lwr) {
            const auto full=w.value("icmp eq i32 "+w.binary("and","i32",address,std::to_string(access.bytes-1))+", 0");
            const auto kept=w.binary("and","i64",w.reg(i.rt),"18446744069414584320");
            const auto merged=w.binary("or","i64",kept,w.value("zext i32 "+value+" to i64"));
            value=w.value("select i1 "+full+", i64 "+w.extend(value)+", i64 "+merged);
        } else if (access.bytes<8) value=w.value(std::string(access.sign ? "sext " : "zext ")+type+" "+value+" to i64");
        if (i.rt) w.store(access.bytes==16 ? type : "i64",value,offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
    }
    if (native_io) w.out<<"  br label %memory_done"<<suffix<<"\nmemory_done"<<suffix<<":\n";
}
}
