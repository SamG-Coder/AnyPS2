#include <nt/ee_codegen.hpp>
#include "llvm_writer.hpp"
#include "memory.hpp"
#include "dispatch.hpp"
#include "branches.hpp"
#include "system.hpp"
#include <nt/ee_native.hpp>
#include <cstddef>
#include <bit>
#include <vector>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace nt::ee_codegen {
namespace {
using r5900::Op;
bool supported(Op op) {
    if (memory_operation(op)) return true;
    switch (op) {
    case Op::mfhi1: case Op::mflo1: case Op::mthi1: case Op::mtlo1:
    case Op::mfhi: case Op::mflo: case Op::mthi: case Op::mtlo:
    case Op::ei: case Op::di: case Op::sync:
    case Op::adda_s: case Op::add_s: case Op::sub_s: case Op::mul_s:
    case Op::cfc1: case Op::ctc1:
    case Op::mfc1: case Op::mtc1:
    case Op::mfsa: case Op::mtsab: case Op::mtsah:
    case Op::slt: case Op::sltu: case Op::slti: case Op::sltiu:
    case Op::beql: case Op::bnel: case Op::blez: case Op::bgtz: case Op::bltz: case Op::bgez:
    case Op::blezl: case Op::bgtzl: case Op::bltzl: case Op::bgezl:
    case Op::dsll: case Op::dsrl: case Op::dsra: case Op::dsll32: case Op::dsrl32: case Op::dsra32:
    case Op::dsllv: case Op::dsrlv: case Op::dsrav: case Op::sllv: case Op::srlv: case Op::srav:
    case Op::add: case Op::sub: case Op::addi: case Op::dadd: case Op::dsub: case Op::daddi:
    case Op::ppacb: case Op::ppach: case Op::ppacw: case Op::pextlb: case Op::pextlh: case Op::pextlw: case Op::pextub: case Op::pextuh: case Op::pextuw:
    case Op::ppac5: case Op::pext5:
    case Op::pceqb: case Op::pceqh: case Op::pceqw:
    case Op::pand: case Op::por: case Op::pxor: case Op::pnor:
    case Op::pcpyld: case Op::pcpyud: case Op::pcpyh:
    case Op::plzcw:
    case Op::vmove: case Op::vmr32:
    case Op::vadd: case Op::vsub: case Op::vadd_bc: case Op::vsub_bc:
    case Op::breakpoint:
    case Op::mult: case Op::multu: case Op::div: case Op::divu:
    case Op::mfc0: case Op::mtc0:
    case Op::cfc2: case Op::ctc2:
    case Op::mov_s: case Op::abs_s: case Op::neg_s:
    case Op::compare_s: case Op::bc1f: case Op::bc1t: case Op::bc1fl: case Op::bc1tl:
    case Op::movz: case Op::movn:
    case Op::padduw:
    case Op::j: case Op::jal: case Op::jr: case Op::jalr: case Op::beq: case Op::bne:
    case Op::sll: case Op::srl: case Op::sra: case Op::addiu: case Op::daddiu:
    case Op::addu: case Op::subu: case Op::daddu: case Op::dsubu:
    case Op::bit_and: case Op::bit_or: case Op::bit_xor: case Op::bit_nor:
    case Op::andi: case Op::ori: case Op::xori: case Op::lui: case Op::syscall: return true;
    default: return false;
    }
}

void emit_part(Writer& w,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> ranges,std::size_t index) {
    w.out<<"define internal i32 @nt_ee_part_"<<index<<"(ptr %state, ptr %memory, i64 %budget) noinline {\nentry:\n"
         <<"  %remaining = alloca i64\n  store i64 %budget, ptr %remaining\n  br label %dispatch\ndispatch:\n";
    const auto pc=w.load("i32",offsetof(runtime::State,pc));
    w.out<<"  switch i32 "<<pc<<", label %unmapped [\n";
    for (const auto& i:instructions) w.out<<"    i32 "<<i.pc<<", label %block"<<i.pc<<"\n";
    w.out<<"  ]\n";
    for (std::size_t n=0;n<instructions.size();++n) {
        const auto& i=instructions[n]; const auto suffix=std::to_string(i.pc);
        w.out<<"block"<<suffix<<":\n";
        const auto delayed=w.load("i8",offsetof(runtime::State,delay_slot));
        const auto is_delay=w.value("icmp ne i8 "+delayed+", 0");
        std::string invalid=is_delay;
        if (n && instructions[n-1].pc+4==i.pc && instructions[n-1].branch()) {
            const auto origin=w.load("i32",offsetof(runtime::State,branch_pc));
            const auto wrong=w.value("icmp ne i32 "+origin+", "+std::to_string(instructions[n-1].pc));
            invalid=w.binary("and","i1",is_delay,wrong);
        }
        w.out<<"  br i1 "<<invalid<<", label %invalid, label %check"<<suffix<<"\ncheck"<<suffix<<":\n";
        const auto remaining=w.value("load i64, ptr %remaining");
        const auto empty=w.value("icmp eq i64 "+remaining+", 0");
        w.out<<"  br i1 "<<empty<<", label %budget_stop, label %execute"<<suffix<<"\nexecute"<<suffix<<":\n";
        const auto status=w.load("i32",offsetof(runtime::State,cop0_status));
        const auto gate=w.binary("and","i32",status,"65543");
        const auto enabled=w.value("icmp eq i32 "+gate+", 65537");
        const auto events=w.load("i32",offsetof(ee::Memory,pending_interrupts),"%memory");
        const auto masked=w.binary("and","i32",w.binary("and","i32",events,status),"65280");
        const auto ready=w.value("icmp ne i32 "+masked+", 0");
        const auto boundary=w.binary("xor","i1",is_delay,"true");
        const auto deliver=w.binary("and","i1",w.binary("and","i1",enabled,ready),boundary);
        w.out<<"  br i1 "<<deliver<<", label %interrupt_stop, label %body"<<suffix<<"\nbody"<<suffix<<":\n";
        if (i.op==Op::breakpoint) { w.out<<"  ret i32 11\n"; continue; }
        if (i.op==Op::syscall) { w.out<<"  ret i32 1\n"; continue; }
        std::string taken="true";
        if (i.branch()) {
            taken=branch_condition(w,i);
            // Capture operands before writing the link or executing the delay slot.
            std::string target;
            if (i.op==Op::jr || i.op==Op::jalr) target=w.narrow(w.reg(i.rs));
            else target=w.value("select i1 "+taken+", i32 "+std::to_string(i.target())+", i32 "+std::to_string(i.pc+8));
            if (i.op==Op::jal || (i.op==Op::jalr && i.rd)) {
                const auto reg=i.op==Op::jal ? 31U : i.rd;
                w.store("i64",w.extend(std::to_string(i.pc+8)),offsetof(runtime::State,gpr)+reg*sizeof(runtime::Register));
            }
            w.store("i32",target,offsetof(runtime::State,pending_target));
            w.store("i32",suffix,offsetof(runtime::State,branch_pc));
            w.store("i8",i.likely() ? w.value("zext i1 "+taken+" to i8") : "1",offsetof(runtime::State,delay_slot));
            const auto successor=i.likely() ? w.value("select i1 "+taken+", i32 "+std::to_string(i.pc+4)+", i32 "+std::to_string(i.pc+8)) : std::to_string(i.pc+4);
            w.store("i32",successor,offsetof(runtime::State,pc));
        } else {
            if (const auto access=memory_operation(i.op)) emit_memory(w,i,*access,ranges);
            else if (i.op==Op::mfc0 || i.op==Op::mtc0) emit_system_transfer(w,i);
            else w.instruction(i);
            const auto pending=w.load("i32",offsetof(runtime::State,pending_target));
            const auto successor=w.value("select i1 "+is_delay+", i32 "+pending+", i32 "+std::to_string(i.pc+4));
            w.store("i32",successor,offsetof(runtime::State,pc));
            w.store("i8","0",offsetof(runtime::State,delay_slot));
        }
        const auto count=w.load("i64",offsetof(runtime::State,executed));
        w.store("i64",w.binary("add","i64",count,"1"),offsetof(runtime::State,executed));
        const auto left=w.binary("sub","i64",remaining,"1");
        w.out<<"  store i64 "<<left<<", ptr %remaining\n";
        const auto sequential=n+1<instructions.size() && instructions[n+1].pc==i.pc+4 ? "%block"+std::to_string(instructions[n+1].pc) : "%unmapped";
        if (i.likely()) w.out<<"  br i1 "<<taken<<", label "<<sequential<<", label %dispatch\n";
        else if (!i.branch() && n && instructions[n-1].pc+4==i.pc && instructions[n-1].branch())
            w.out<<"  br i1 "<<is_delay<<", label %dispatch, label "<<sequential<<"\n";
        else w.out<<"  br label "<<sequential<<"\n";
    }
    w.out<<"interrupt_stop:\n  ret i32 7\nbudget_stop:\n  ret i32 0\nunmapped:\n  ret i32 2\ninvalid:\n  ret i32 3\n}\n";
}

}
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> protected_code) {
    static_assert(std::is_standard_layout_v<runtime::State> && std::is_standard_layout_v<ee::Memory>);
    static_assert(sizeof(void*)==8 && std::endian::native==std::endian::little,
                  "EE LLVM currently requires a little-endian 64-bit host ABI");
    if (instructions.empty() || instructions.size()>program_instruction_limit) throw std::invalid_argument("EE program exceeds the bounded instruction limit or is empty");
    for (std::size_t n=0;n<instructions.size();++n) {
        const auto& i=instructions[n];
        const auto decoded=r5900::decode(i.pc,i.word);
        if (i.pc%4 || (n && i.pc<=instructions[n-1].pc) ||
            i.op!=decoded.op || i.rs!=decoded.rs || i.rt!=decoded.rt || i.rd!=decoded.rd || i.sa!=decoded.sa || i.immediate!=decoded.immediate)
            throw std::invalid_argument("EE region contains inconsistent decoded instructions");
        if (!supported(i.op)) {
            std::ostringstream diagnostic;
            diagnostic<<"EE LLVM unsupported operation: "<<r5900::name(i.op)<<" at 0x"<<std::hex<<i.pc;
            throw std::runtime_error(diagnostic.str());
        }
        if ((i.op==Op::mfc0 || i.op==Op::mtc0) && i.rd!=25 && i.rd!=12 && i.rd!=14 && i.rd!=30)
            throw std::invalid_argument("EE system transfer selector is not supported");
        if (i.op==Op::cfc2 && i.rd==20)
            throw std::invalid_argument("EE vector random-register transfer is not implemented");
        if ((i.op==Op::ctc1 || i.op==Op::cfc1) && i.rd!=31)
            throw std::invalid_argument("EE floating control transfer supports FCR31 only");
        if (i.op==Op::sync && (i.rs || i.rt || i.rd || (i.sa!=0 && i.sa!=16)))
            throw std::invalid_argument("Unsupported EE SYNC encoding");
        if (i.branch() && (n+1==instructions.size() || instructions[n+1].pc!=i.pc+4 || instructions[n+1].branch() || instructions[n+1].op==Op::syscall))
            throw std::invalid_argument("EE branch requires a supported scalar delay slot");
    }
    std::vector<CodeRange> ranges(protected_code.begin(),protected_code.end());
    for (const auto& range:ranges)
        if (!range.size || std::uint64_t(range.address)+range.size>0x100000000ULL)
            throw std::invalid_argument("Invalid EE executable protection extent");
    for (const auto& i:instructions) {
        const auto address=physical_address(i.pc);
        if (!ranges.empty() && std::uint64_t(ranges.back().address)+ranges.back().size==address)
            ranges.back().size+=4;
        else ranges.push_back({address,4});
    }
    std::sort(ranges.begin(),ranges.end(),[](const auto& a,const auto& b) { return a.address<b.address; });
    std::vector<CodeRange> merged;
    for (const auto& range:ranges) {
        if (!merged.empty()) {
            auto& previous=merged.back();
            const auto previous_end=std::uint64_t(previous.address)+previous.size;
            const auto end=std::max(previous_end,std::uint64_t(range.address)+range.size);
            if (range.address<=previous_end && end-previous.address<=0xffffffffULL) {
                previous.size=static_cast<std::uint32_t>(end-previous.address); continue;
            }
        }
        merged.push_back(range);
    }
    ranges=std::move(merged);
    std::vector<std::span<const r5900::Instruction>> parts;
    for (std::size_t first=0;first<instructions.size();) {
        auto count=std::min(function_instruction_limit,instructions.size()-first);
        if (instructions[first+count-1].branch()) --count;
        parts.push_back(instructions.subspan(first,count)); first+=count;
    }
    Writer w;
    for (std::size_t index=0;index<parts.size();++index) emit_part(w,parts[index],ranges,index);
    emit_dispatcher(w,parts);
    w.out<<"define i1 @nt_ee_generated_code_overlap(i32 %address, i32 %bytes) {\nentry:\n";
    const auto address=w.value("zext i32 %address to i64");
    const auto bytes=w.value("zext i32 %bytes to i64");
    const auto end=w.binary("add","i64",address,bytes);
    const auto overlap=code_overlap(w,address,end,ranges);
    w.out<<"  ret i1 "<<overlap<<"\n}\n";
    w.out<<"declare <4 x i32> @llvm.uadd.sat.v4i32(<4 x i32>, <4 x i32>)\n";
    w.out<<"declare i64 @nt_ee_add_finite(i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_vector_read(ptr, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_vector_write(ptr, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_multiply_finite(i32, i32)\n";
    w.out<<"declare i32 @nt_ee_vector_add(ptr, i32, i32, i32, i32, i32, i32)\n";
    w.out<<"declare <2 x i32> @llvm.ctlz.v2i32(<2 x i32>, i1 immarg)\n";
    for (const auto width : {32,64}) for (const auto operation : {"sadd","ssub"})
        w.out<<"declare { i"<<width<<", i1 } @llvm."<<operation<<".with.overflow.i"<<width<<"(i"<<width<<", i"<<width<<")\n";
    output<<w.out.str();
}
}
