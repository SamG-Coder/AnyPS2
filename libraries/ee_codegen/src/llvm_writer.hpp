#pragma once
#include <nt/ee_codegen.hpp>
#include <nt/ee_native.hpp>
#include <cstddef>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen {
using r5900::Op;
struct Writer {
    std::ostream& out;
    unsigned next=0;
    bool shared_code_guard=false;
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
    std::string vector_storage(const std::string& suffix) {
        const auto vectors=load("ptr",offsetof(ee::Memory,vectors),"%memory");
        const auto missing=value("icmp eq ptr "+vectors+", null");
        out<<"  br i1 "<<missing<<", label %vector_missing"<<suffix<<", label %vector_check"<<suffix
             <<"\nvector_missing"<<suffix<<":\n  ret i32 9\nvector_check"<<suffix<<":\n";
        const auto active=load("i8",offsetof(ee::VectorStorage,active),vectors);
        const auto busy=value("icmp ne i8 "+active+", 0");
        out<<"  br i1 "<<busy<<", label %vector_busy"<<suffix<<", label %vector_access"<<suffix
             <<"\nvector_busy"<<suffix<<":\n  ret i32 8\nvector_access"<<suffix<<":\n";
        return vectors;
    }
    std::string vu_reg(const std::string& vectors,unsigned index) {
        return index ? load("<4 x i32>",offsetof(ee::VectorStorage,values)+(index-1)*sizeof(std::array<std::uint32_t,4>),vectors) :
            "<i32 0, i32 0, i32 0, i32 1065353216>";
    }
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
        case Op::ei: case Op::di: {
            const auto status=load("i32",offsetof(runtime::State,cop0_status));
            const auto override_bits=binary("and","i32",status,"131078");
            const auto override_enabled=value("icmp ne i32 "+override_bits+", 0");
            const auto mode=binary("and","i32",status,"24");
            const auto kernel=value("icmp eq i32 "+mode+", 0");
            const auto allowed=binary("or","i1",override_enabled,kernel);
            const auto changed=binary(i.op==Op::ei ? "or" : "and","i32",status,
                                      i.op==Op::ei ? "65536" : "4294901759");
            store("i32",value("select i1 "+allowed+", i32 "+changed+", i32 "+status),
                  offsetof(runtime::State,cop0_status));
            return;
        }
        case Op::mtsa:
            store("i64",value("zext i32 "+narrow(reg(i.rs))+" to i64"),offsetof(runtime::State,sa));
            return;
        case Op::qmfc2: case Op::qmtc2: {
            const auto vectors=vector_storage(std::to_string(i.pc));
            if (i.op==Op::qmfc2 && i.rt)
                store("<4 x i32>",vu_reg(vectors,i.rd),offsetof(runtime::State,gpr)+i.rt*sizeof(runtime::Register));
            if (i.op==Op::qmtc2 && i.rd)
                store("<4 x i32>",vector_reg(i.rt),offsetof(ee::VectorStorage,values)+(i.rd-1)*sizeof(std::array<std::uint32_t,4>),vectors);
            return;
        }
        case Op::pref:
            // A non-faulting locality hint: the native optimizer owns placement.
            return;
        case Op::sync:
            out<<"  fence seq_cst\n"; return;
        case Op::cfc2: case Op::ctc2: {
            const bool write=i.op==Op::ctc2;
            const auto arguments="ptr %memory, i32 "+std::to_string(i.rd)+", i32 "+std::to_string(i.word&1U);
            const auto packed=value(write ? "call i64 @nt_ee_vector_write("+arguments+", i32 "+narrow(reg(i.rt))+")" :
                                             "call i64 @nt_ee_vector_read("+arguments+")");
            const auto status=binary("lshr","i64",packed,"32");
            const auto ready=value("icmp eq i64 "+status+", 0");
            const auto suffix=std::to_string(i.pc);
            out<<"  br i1 "<<ready<<", label %vector_ready"<<suffix<<", label %vector_stop"<<suffix
               <<"\nvector_stop"<<suffix<<":\n";
            const auto pending=value("icmp eq i64 "+status+", 1");
            const auto stop=value("select i1 "+pending+", i32 8, i32 9");
            out<<"  ret i32 "<<stop<<"\nvector_ready"<<suffix<<":\n";
            if (write) return;
            destination=i.rt; result=extend(narrow(packed)); break;
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
        case Op::ppacb: case Op::ppach: case Op::ppacw: case Op::pextlb: case Op::pextlh: case Op::pextlw: case Op::pextub: case Op::pextuh: case Op::pextuw: {
            const auto width=i.op==Op::ppacb || i.op==Op::pextlb || i.op==Op::pextub ? 8U :
                             i.op==Op::ppach || i.op==Op::pextlh || i.op==Op::pextuh ? 16U : 32U;
            const bool pack=i.op==Op::ppacb || i.op==Op::ppach || i.op==Op::ppacw;
            const bool upper=i.op==Op::pextub || i.op==Op::pextuh || i.op==Op::pextuw;
            const auto lanes=128/width;
            const auto type="<"+std::to_string(lanes)+" x i"+std::to_string(width)+">";
            const auto source=[&](unsigned index) {
                const auto raw=vector_reg(index); return width==32 ? raw : value("bitcast <4 x i32> "+raw+" to "+type);
            };
            const auto right=source(i.rt),left=source(i.rs);
            std::string mask="<";
            for (unsigned lane=0;lane<lanes;++lane) {
                const auto index=pack ? (lane/(lanes/2))*lanes+(lane%(lanes/2))*2 :
                                       (lane%2)*lanes+lane/2+(upper ? lanes/2 : 0);
                if (lane) mask+=", "; mask+="i32 "+std::to_string(index);
            }
            mask+=">";
            const auto shuffled=value("shufflevector "+type+" "+right+", "+type+" "+left+", <"+std::to_string(lanes)+" x i32> "+mask);
            if (i.rd) store(type,shuffled,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::ppac5: case Op::pext5: {
            const auto source=vector_reg(i.rt);
            const auto splat=[](unsigned bits) {
                const auto lane="i32 "+std::to_string(bits);
                return "<"+lane+", "+lane+", "+lane+", "+lane+">";
            };
            std::string combined="zeroinitializer";
            constexpr unsigned masks[]{31,992,31744,32768},shifts[]{3,6,9,16};
            for (unsigned component=0;component<4;++component) {
                const auto masked=i.op==Op::ppac5 ? binary("and","<4 x i32>",binary("lshr","<4 x i32>",source,splat(shifts[component])),splat(masks[component])) :
                    binary("shl","<4 x i32>",binary("and","<4 x i32>",source,splat(masks[component])),splat(shifts[component]));
                combined=binary("or","<4 x i32>",combined,masked);
            }
            if (i.rd) store("<4 x i32>",combined,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::pceqb: case Op::pceqh: case Op::pceqw: {
            const auto width=i.op==Op::pceqb ? 8U : i.op==Op::pceqh ? 16U : 32U;
            const auto lanes=std::to_string(128/width);
            const auto type="<"+lanes+" x i"+std::to_string(width)+">";
            const auto convert=[&](unsigned index) {
                const auto raw=vector_reg(index);
                return width==32 ? raw : value("bitcast <4 x i32> "+raw+" to "+type);
            };
            const auto left=convert(i.rs),right=convert(i.rt);
            const auto equal=value("icmp eq "+type+" "+left+", "+right);
            const auto masks=value("sext <"+lanes+" x i1> "+equal+" to "+type);
            if (i.rd) store(type,masks,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::pand: case Op::por: case Op::pxor: case Op::pnor: {
            const auto left=vector_reg(i.rs),right=vector_reg(i.rt);
            auto bits=binary(i.op==Op::pand ? "and" : i.op==Op::pxor ? "xor" : "or","<4 x i32>",left,right);
            if (i.op==Op::pnor) bits=binary("xor","<4 x i32>",bits,"<i32 -1, i32 -1, i32 -1, i32 -1>");
            if (i.rd) store("<4 x i32>",bits,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::pcpyld: case Op::pcpyud: case Op::pcpyh: {
            const auto right=vector_reg(i.rt);
            std::string copied;
            if (i.op==Op::pcpyh) {
                const auto halves=value("bitcast <4 x i32> "+right+" to <8 x i16>");
                const auto repeated=value("shufflevector <8 x i16> "+halves+", <8 x i16> poison, <8 x i32> <i32 0, i32 0, i32 0, i32 0, i32 4, i32 4, i32 4, i32 4>");
                copied=value("bitcast <8 x i16> "+repeated+" to <4 x i32>");
            } else {
                const auto left=vector_reg(i.rs);
                copied=i.op==Op::pcpyld ? value("shufflevector <4 x i32> "+right+", <4 x i32> "+left+", <4 x i32> <i32 0, i32 1, i32 4, i32 5>") :
                    value("shufflevector <4 x i32> "+left+", <4 x i32> "+right+", <4 x i32> <i32 2, i32 3, i32 6, i32 7>");
            }
            if (i.rd) store("<4 x i32>",copied,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::padduw: {
            const auto left=vector_reg(i.rs),right=vector_reg(i.rt);
            const auto sum=value("call <4 x i32> @llvm.uadd.sat.v4i32(<4 x i32> "+left+", <4 x i32> "+right+")");
            if (i.rd) store("<4 x i32>",sum,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
        case Op::slt: case Op::sltu: case Op::slti: case Op::sltiu: {
            const bool immediate=i.op==Op::slti || i.op==Op::sltiu;
            const bool unsigned_compare=i.op==Op::sltu || i.op==Op::sltiu;
            destination=immediate ? i.rt : i.rd;
            const auto right=immediate ? std::to_string(static_cast<std::int64_t>(i.immediate)) : reg(i.rt);
            const auto comparison=value(std::string(unsigned_compare ? "icmp ult i64 " : "icmp slt i64 ")+reg(i.rs)+", "+right);
            result=value("zext i1 "+comparison+" to i64"); break;
        }
        case Op::plzcw: {
            const auto words=value("bitcast i64 "+reg(i.rs)+" to <2 x i32>");
            const auto signs=value("ashr <2 x i32> "+words+", <i32 31, i32 31>");
            const auto magnitude=value("xor <2 x i32> "+words+", "+signs);
            const auto counts=value("call <2 x i32> @llvm.ctlz.v2i32(<2 x i32> "+magnitude+", i1 false)");
            const auto adjusted=value("sub <2 x i32> "+counts+", <i32 1, i32 1>");
            result=value("bitcast <2 x i32> "+adjusted+" to i64"); break;
        }
        case Op::movz: case Op::movn: {
            const auto condition=value(std::string(i.op==Op::movz ? "icmp eq i64 " : "icmp ne i64 ")+reg(i.rt)+", 0");
            result=value("select i1 "+condition+", i64 "+reg(i.rs)+", i64 "+reg(i.rd)); break;
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
        case Op::add: case Op::sub: case Op::addi: case Op::dadd: case Op::dsub: case Op::daddi: {
            const bool word=i.op==Op::add || i.op==Op::sub || i.op==Op::addi;
            const bool immediate=i.op==Op::addi || i.op==Op::daddi;
            const bool subtract=i.op==Op::sub || i.op==Op::dsub;
            const std::string type=word ? "i32" : "i64";
            const auto left=word ? narrow(reg(i.rs)) : reg(i.rs);
            const auto right=immediate ? std::to_string(i.immediate) : word ? narrow(reg(i.rt)) : reg(i.rt);
            const auto aggregate="{ "+type+", i1 }";
            const auto checked=value("call "+aggregate+" @llvm."+(subtract ? "ssub" : "sadd")+".with.overflow."+type+"("+type+" "+left+", "+type+" "+right+")");
            const auto overflow=value("extractvalue "+aggregate+" "+checked+", 1");
            const auto suffix=std::to_string(i.pc);
            out<<"  br i1 "<<overflow<<", label %overflow"<<suffix<<", label %arithmetic"<<suffix
               <<"\noverflow"<<suffix<<":\n  ret i32 12\narithmetic"<<suffix<<":\n";
            result=value("extractvalue "+aggregate+" "+checked+", 0");
            if (word) result=extend(result);
            if (immediate) destination=i.rt;
            break;
        }
        case Op::addu: case Op::subu:
            result=extend(binary(i.op==Op::addu ? "add" : "sub","i32",narrow(reg(i.rs)),narrow(reg(i.rt)))); break;
        case Op::daddu: case Op::dsubu:
            result=binary(i.op==Op::daddu ? "add" : "sub","i64",reg(i.rs),reg(i.rt)); break;
        case Op::dsll: case Op::dsrl: case Op::dsra: case Op::dsll32: case Op::dsrl32: case Op::dsra32:
        case Op::dsllv: case Op::dsrlv: case Op::dsrav:
        case Op::sllv: case Op::srlv: case Op::srav: case Op::sll: case Op::srl: case Op::sra: {
            const bool word=i.op==Op::sll || i.op==Op::srl || i.op==Op::sra ||
                            i.op==Op::sllv || i.op==Op::srlv || i.op==Op::srav;
            const bool variable=i.op==Op::sllv || i.op==Op::srlv || i.op==Op::srav ||
                                i.op==Op::dsllv || i.op==Op::dsrlv || i.op==Op::dsrav;
            const bool high=i.op==Op::dsll32 || i.op==Op::dsrl32 || i.op==Op::dsra32;
            const bool left=i.op==Op::sll || i.op==Op::sllv || i.op==Op::dsll || i.op==Op::dsll32 || i.op==Op::dsllv;
            const bool logical=i.op==Op::srl || i.op==Op::srlv || i.op==Op::dsrl || i.op==Op::dsrl32 || i.op==Op::dsrlv;
            auto amount=variable ? binary("and","i64",reg(i.rs),word ? "31" : "63") : std::to_string(i.sa+(high ? 32 : 0));
            if (word && variable) amount=narrow(amount);
            result=binary(left ? "shl" : logical ? "lshr" : "ashr",word ? "i32" : "i64",
                          word ? narrow(reg(i.rt)) : reg(i.rt),amount);
            if (word) result=extend(result);
            break;
        }
        case Op::bit_and: case Op::bit_or: case Op::bit_xor: case Op::bit_nor:
            result=binary(i.op==Op::bit_and ? "and" : i.op==Op::bit_xor ? "xor" : "or","i64",reg(i.rs),reg(i.rt));
            if (i.op==Op::bit_nor) result=binary("xor","i64",result,"-1"); break;
        default: throw std::logic_error("Unvalidated EE operation");
        }
        if (destination) store("i64",result,offsetof(runtime::State,gpr)+destination*sizeof(runtime::Register));
    }
};
}
