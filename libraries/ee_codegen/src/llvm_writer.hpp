#pragma once
#include <nt/ee_codegen.hpp>
#include <nt/ee_native.hpp>
#include <cstddef>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen {
using r5900::Op;
struct Writer {
    std::ostringstream out;
    unsigned next=0;
    std::string value(const std::string& expression) {
        const auto id="%v"+std::to_string(next++); out<<"  "<<id<<" = "<<expression<<"\n"; return id;
    }
    std::string pointer(std::size_t offset,const std::string& base="%state") { return value("getelementptr i8, ptr "+base+", i64 "+std::to_string(offset)); }
    std::string load(const std::string& type,std::size_t offset,const std::string& base="%state") { return value("load "+type+", ptr "+pointer(offset,base)+", align 1"); }
    void store(const std::string& type,const std::string& v,std::size_t offset,const std::string& base="%state") {
        const auto p=pointer(offset,base); out<<"  store "<<type<<" "<<v<<", ptr "<<p<<", align 1\n";
    }
    std::string reg(unsigned index) {
        return index ? load("i64",offsetof(runtime::State,gpr)+index*sizeof(runtime::Register)) : "0";
    }
    std::string vector_reg(unsigned index) {
        return index ? load("<4 x i32>",offsetof(runtime::State,gpr)+index*sizeof(runtime::Register)) : "zeroinitializer";
    }
    std::string binary(const char* operation,const std::string& type,const std::string& a,const std::string& b) {
        return value(std::string(operation)+" "+type+" "+a+", "+b);
    }
    std::string narrow(const std::string& v) { return value("trunc i64 "+v+" to i32"); }
    std::string extend(const std::string& v) { return value("sext i32 "+v+" to i64"); }
    static std::size_t accumulator_offset(Op op) {
        switch (op) {
        case Op::mthi: case Op::mfhi: return offsetof(runtime::State,hi);
        case Op::mtlo: case Op::mflo: return offsetof(runtime::State,lo);
        case Op::mthi1: case Op::mfhi1: return offsetof(runtime::State,hi1);
        case Op::mtlo1: case Op::mflo1: return offsetof(runtime::State,lo1);
        default: throw std::logic_error("Invalid accumulator transfer");
        }
    }
    void instruction(const r5900::Instruction& i) {
        unsigned destination=i.rd;
        std::string result;
        switch (i.op) {
        case Op::mthi: case Op::mtlo: case Op::mthi1: case Op::mtlo1:
            store("i64",reg(i.rs),accumulator_offset(i.op));
            return;
        case Op::mfhi: case Op::mflo: case Op::mfhi1: case Op::mflo1:
            result=load("i64",accumulator_offset(i.op)); break;
        case Op::sync:
            out<<"  fence seq_cst\n"; return;
        case Op::adda_s: case Op::add_s: case Op::sub_s: {
            const auto left=load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
            const auto right=load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
            const auto sum=value("call i64 @nt_ee_add_finite(i32 "+left+", i32 "+right+", i32 "+(i.op==Op::sub_s ? "1" : "0")+")");
            const auto unsupported=value("icmp uge i64 "+sum+", 8589934592");
            const auto suffix=std::to_string(i.pc);
            out<<"  br i1 "<<unsupported<<", label %numeric_stop"<<suffix<<", label %numeric_result"<<suffix
               <<"\nnumeric_stop"<<suffix<<":\n  ret i32 6\nnumeric_result"<<suffix<<":\n";
            const auto underflow=value("icmp uge i64 "+sum+", 4294967296");
            const auto flags=load("i32",offsetof(runtime::State,fcr31));
            const auto cleared=binary("and","i32",flags,"4294918143");
            const auto added=value("select i1 "+underflow+", i32 16392, i32 0");
            store("i32",narrow(sum),i.op==Op::adda_s ? offsetof(runtime::State,accumulator) :
                  offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
            store("i32",binary("or","i32",cleared,added),offsetof(runtime::State,fcr31));
            return;
        }
        case Op::ctc1: {
            constexpr std::uint32_t writable=0x0083c078U;
            const auto flags=load("i32",offsetof(runtime::State,fcr31));
            const auto kept=binary("and","i32",flags,std::to_string(~writable));
            const auto changed=binary("and","i32",narrow(reg(i.rt)),std::to_string(writable));
            store("i32",binary("or","i32",kept,changed),offsetof(runtime::State,fcr31)); return;
        }
        case Op::cfc1:
            destination=i.rt; result=extend(load("i32",offsetof(runtime::State,fcr31))); break;
        case Op::mtc1:
            store("i32",narrow(reg(i.rt)),offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t)); return;
        case Op::mfc1:
            destination=i.rt; result=extend(load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t))); break;
        case Op::mfsa:
            result=load("i64",offsetof(runtime::State,sa)); break;
        case Op::mtsab: case Op::mtsah: {
            const auto mask=i.op==Op::mtsab ? 15U : 7U;
            const auto source=binary("and","i64",reg(i.rs),std::to_string(mask));
            auto amount=binary("xor","i64",source,std::to_string(std::uint32_t(i.immediate)&mask));
            if (i.op==Op::mtsah) amount=binary("shl","i64",amount,"1");
            store("i64",amount,offsetof(runtime::State,sa)); return;
        }
        case Op::padduw: {
            const auto left=vector_reg(i.rs),right=vector_reg(i.rt);
            const auto sum=value("call <4 x i32> @llvm.uadd.sat.v4i32(<4 x i32> "+left+", <4 x i32> "+right+")");
            if (i.rd) store("<4 x i32>",sum,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::lui:
            destination=i.rt; result=extend(std::to_string(std::uint32_t(i.immediate)<<16)); break;
        case Op::addiu: case Op::daddiu: {
            destination=i.rt; const auto source=reg(i.rs);
            result=i.op==Op::addiu ? extend(binary("add","i32",narrow(source),std::to_string(i.immediate))) :
                binary("add","i64",source,std::to_string(i.immediate)); break;
        }
        case Op::andi: case Op::ori: case Op::xori:
            destination=i.rt; result=binary(i.op==Op::andi ? "and" : i.op==Op::ori ? "or" : "xor",
                "i64",reg(i.rs),std::to_string(std::uint32_t(i.immediate)&65535)); break;
        case Op::addu: case Op::subu:
            result=extend(binary(i.op==Op::addu ? "add" : "sub","i32",narrow(reg(i.rs)),narrow(reg(i.rt)))); break;
        case Op::daddu: case Op::dsubu:
            result=binary(i.op==Op::daddu ? "add" : "sub","i64",reg(i.rs),reg(i.rt)); break;
        case Op::sll: case Op::srl: case Op::sra:
            result=extend(binary(i.op==Op::sll ? "shl" : i.op==Op::srl ? "lshr" : "ashr",
                "i32",narrow(reg(i.rt)),std::to_string(i.sa))); break;
        case Op::bit_and: case Op::bit_or: case Op::bit_xor: case Op::bit_nor:
            result=binary(i.op==Op::bit_and ? "and" : i.op==Op::bit_xor ? "xor" : "or","i64",reg(i.rs),reg(i.rt));
            if (i.op==Op::bit_nor) result=binary("xor","i64",result,"-1"); break;
        default: throw std::logic_error("Unvalidated EE operation");
        }
        if (destination) store("i64",result,offsetof(runtime::State,gpr)+destination*sizeof(runtime::Register));
    }
};
}
