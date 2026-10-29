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
        case Op::mult: case Op::multu: case Op::div: case Op::divu: {
            const bool signed_operation=i.op==Op::mult || i.op==Op::div;
            const bool multiply=i.op==Op::mult || i.op==Op::multu;
            const auto operand=[&](unsigned r) {
                const auto low=narrow(reg(r));
                return signed_operation ? extend(low) : value("zext i32 "+low+" to i64");
            };
            const auto a=operand(i.rs),b=operand(i.rt);
            std::string quotient,remainder;
            if (multiply) {
                quotient=binary("mul","i64",a,b);
                remainder=binary("lshr","i64",quotient,"32");
            } else {
                const auto zero=value("icmp eq i64 "+b+", 0");
                const auto denominator=value("select i1 "+zero+", i64 1, i64 "+b);
                const auto divided=binary(signed_operation ? "sdiv" : "udiv","i64",a,denominator);
                const auto modulus=binary(signed_operation ? "srem" : "urem","i64",a,denominator);
                const auto fallback=signed_operation ? value("select i1 "+value("icmp slt i64 "+a+", 0")+", i64 1, i64 -1") : "-1";
                quotient=value("select i1 "+zero+", i64 "+fallback+", i64 "+divided);
                remainder=value("select i1 "+zero+", i64 "+a+", i64 "+modulus);
            }
            const auto lo=extend(narrow(quotient)),hi=extend(narrow(remainder));
            store("i64",lo,offsetof(runtime::State,lo)); store("i64",hi,offsetof(runtime::State,hi));
            if (multiply && i.rd) store("i64",lo,offsetof(runtime::State,gpr)+i.rd*sizeof(runtime::Register));
            return;
        }
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
        case Op::sync:
            out<<"  fence seq_cst\n"; return;
        case Op::adda_s: case Op::add_s: case Op::sub_s: case Op::mul_s: {
            const auto left=load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
            const auto right=load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
            const auto sum=value(i.op==Op::mul_s ? "call i64 @nt_ee_multiply_finite(i32 "+left+", i32 "+right+")" : "call i64 @nt_ee_add_finite(i32 "+left+", i32 "+right+", i32 "+(i.op==Op::sub_s ? "1" : "0")+")");
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
        case Op::mov_s: case Op::abs_s: case Op::neg_s: {
            auto bits=load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
            if (i.op==Op::abs_s) bits=binary("and","i32",bits,"2147483647");
            if (i.op==Op::neg_s) bits=binary("xor","i32",bits,"2147483648");
            store("i32",bits,offsetof(runtime::State,fpr)+i.sa*sizeof(std::uint32_t));
            if (i.op!=Op::mov_s) {
                const auto flags=load("i32",offsetof(runtime::State,fcr31));
                store("i32",binary("and","i32",flags,"4294918143"),offsetof(runtime::State,fcr31));
            }
            return;
        }
        case Op::compare_s: {
            const auto predicate=i.word&15U;
            std::string condition="false";
            if (predicate) {
                const auto left=load("i32",offsetof(runtime::State,fpr)+i.rd*sizeof(std::uint32_t));
                const auto right=load("i32",offsetof(runtime::State,fpr)+i.rt*sizeof(std::uint32_t));
                const auto le=binary("and","i32",left,"2139095040");
                const auto re=binary("and","i32",right,"2139095040");
                const auto invalid=binary("or","i1",value("icmp eq i32 "+le+", 2139095040"),value("icmp eq i32 "+re+", 2139095040"));
                const auto suffix=std::to_string(i.pc);
                out<<"  br i1 "<<invalid<<", label %compare_stop"<<suffix<<", label %compare_values"<<suffix
                   <<"\ncompare_stop"<<suffix<<":\n  ret i32 6\ncompare_values"<<suffix<<":\n";
                const auto normalized=[&](const std::string& bits,const std::string& exponent) {
                    const auto zero=value("icmp eq i32 "+exponent+", 0");
                    const auto finite=value("select i1 "+zero+", i32 0, i32 "+bits);
                    return value("bitcast i32 "+finite+" to float");
                };
                const auto a=normalized(left,le),b=normalized(right,re);
                condition=value(std::string("fcmp ")+(predicate==2 ? "oeq" : predicate==4 ? "olt" : "ole")+" float "+a+", "+b);
            }
            const auto flags=load("i32",offsetof(runtime::State,fcr31));
            const auto kept=binary("and","i32",flags,"4286578687");
            const auto changed=value("select i1 "+condition+", i32 8388608, i32 0");
            store("i32",binary("or","i32",kept,changed),offsetof(runtime::State,fcr31)); return;
        }
        case Op::vmove: case Op::vmr32: {
            const auto vectors=vector_storage(std::to_string(i.pc));
            auto source=vu_reg(vectors,i.rd);
            if (i.op==Op::vmr32) source=value("shufflevector <4 x i32> "+source+", <4 x i32> poison, <4 x i32> <i32 1, i32 2, i32 3, i32 0>");
            std::string mask="<";
            for (unsigned lane=0;lane<4;++lane) {
                if (lane) mask+=", ";
                mask+=std::string("i1 ")+((i.rs&(8U>>lane)) ? "true" : "false");
            }
            mask+=">";
            const auto merged=value("select <4 x i1> "+mask+", <4 x i32> "+source+", <4 x i32> "+vu_reg(vectors,i.rt));
            if (i.rt) store("<4 x i32>",merged,offsetof(ee::VectorStorage,values)+(i.rt-1)*sizeof(std::array<std::uint32_t,4>),vectors);
            return;
        }
        case Op::vadd: case Op::vsub: case Op::vadd_bc: case Op::vsub_bc: {
            const auto subtract=i.op==Op::vsub || i.op==Op::vsub_bc;
            const auto broadcast=i.op==Op::vadd_bc || i.op==Op::vsub_bc ? i.word&3U : 4U;
            const auto outcome=value("call i32 @nt_ee_vector_add(ptr %memory, i32 "+std::to_string(i.sa)+", i32 "+std::to_string(i.rd)+
                ", i32 "+std::to_string(i.rt)+", i32 "+std::to_string(i.rs&15)+", i32 "+(subtract ? "1" : "0")+", i32 "+std::to_string(broadcast)+")");
            const auto ready=value("icmp eq i32 "+outcome+", 0");
            const auto suffix=std::to_string(i.pc);
            out<<"  br i1 "<<ready<<", label %vector_math_ready"<<suffix<<", label %vector_math_stop"<<suffix
               <<"\nvector_math_stop"<<suffix<<":\n  ret i32 "<<outcome<<"\nvector_math_ready"<<suffix<<":\n";
            return;
        }
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
