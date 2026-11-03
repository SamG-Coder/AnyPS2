#include <nt/ee_codegen.hpp>
#include "llvm_writer.hpp"
#include "memory.hpp"
#include "code_protection.hpp"
#include "native_layout.hpp"
#include "images.hpp"
#include "packed.hpp"
#include "vector.hpp"
#include "floating.hpp"
#include "integer_multiply.hpp"
#include "packed_multiply.hpp"
#include "dispatch.hpp"
#include "branches.hpp"
#include "traps.hpp"
#include "patterns.hpp"
#include "system.hpp"
#include <nt/ee_native.hpp>
#include <cstddef>
#include <bit>
#include <vector>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <map>

namespace nt::ee_codegen {
namespace {
using r5900::Op;
bool supported(Op op) {
    if (address_space_operation(op) || memory_operation(op) || packed_operation(op) || integer_multiply(op) ||
        packed_accumulator_transfer(op) || vector_operation(op) || trap_predicate(op) ||
        floating_operation(op) || halfword_product(op)) return true;
    switch (op) {
    case Op::bltzal: case Op::bgezal: case Op::bltzall: case Op::bgezall:
    case Op::mtsa: case Op::qmfc2: case Op::qmtc2:
    case Op::pref: case Op::eret:
    case Op::mfhi1: case Op::mflo1: case Op::mthi1: case Op::mtlo1:
    case Op::mfhi: case Op::mflo: case Op::mthi: case Op::mtlo:
    case Op::ei: case Op::di: case Op::sync:
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
    case Op::breakpoint:
    case Op::mfc0: case Op::mtc0:
    case Op::cfc2: case Op::ctc2:
    case Op::bc1f: case Op::bc1t: case Op::bc1fl: case Op::bc1tl:
    case Op::bc0f: case Op::bc0t: case Op::bc0fl: case Op::bc0tl:
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

void emit_part(Writer& w,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> ranges,std::span<const CodePayload> payloads,const NativeLayout& layout,bool external=false) {
    w.out<<"define "<<(external ? "" : "internal ")<<"i32 @nt_ee_part_"<<instructions.front().pc<<"(ptr %state, ptr %memory, i64 %budget) noinline {\nentry:\n"
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
        for (const auto& range:ranges) if (range.image && i.pc>=range.address && std::uint64_t(i.pc)<std::uint64_t(range.address)+range.size) {
            const auto live=w.load("i32",offsetof(ee::Memory,executable_image),"%memory");
            const auto active=w.value("icmp eq i32 "+live+", "+std::to_string(range.image));
            w.out<<"  br i1 "<<active<<", label %image_ready"<<suffix<<", label %image_stop"<<suffix
                 <<"\nimage_stop"<<suffix<<":\n";
            w.store("i32",std::to_string(i.pc),offsetof(ee::Memory,fault_address),"%memory");
            w.out<<"  ret i32 14\nimage_ready"<<suffix<<":\n";
            break;
        }
        for (std::size_t payload_index=0;payload_index<payloads.size();++payload_index) {
            const auto& payload=payloads[payload_index];
            if (physical_address(i.pc)<payload.address || std::uint64_t(physical_address(i.pc))>=std::uint64_t(payload.address)+payload.bytes.size()) continue;
            const auto data=w.load("ptr",offsetof(ee::Memory,data),"%memory");
            const auto size=w.load("i64",offsetof(ee::Memory,size),"%memory");
            const auto absent=w.value("icmp eq ptr "+data+", null");
            const auto short_memory=w.value("icmp ult i64 "+size+", "+std::to_string(std::uint64_t(payload.address)+payload.bytes.size()));
            const auto invalid_payload=w.binary("or","i1",absent,short_memory);
            w.out<<"  br i1 "<<invalid_payload<<", label %payload_stop"<<suffix<<", label %payload_compare"<<suffix
                 <<"\npayload_compare"<<suffix<<":\n";
            const auto pointer=w.pointer(payload.address,data);
            const auto equal=w.value("call i32 @memcmp(ptr "+pointer+", ptr @nt_payload_"+std::to_string(payload_index)+", i64 "+std::to_string(payload.bytes.size())+")");
            const auto matches=w.value("icmp eq i32 "+equal+", 0");
            w.out<<"  br i1 "<<matches<<", label %payload_ready"<<suffix<<", label %payload_stop"<<suffix
                 <<"\npayload_stop"<<suffix<<":\n";
            w.store("i32",std::to_string(payload.address),offsetof(ee::Memory,fault_address),"%memory");
            w.out<<"  ret i32 14\npayload_ready"<<suffix<<":\n";
        }
        if (display_wait_pattern(instructions.subspan(n)) && !(n && instructions[n-1].branch())) {
            // All four instructions must have the same lifetime/publication
            // guards. The hint never retires code or changes register state.
            bool same_guard=true;
            for (const auto& range:ranges) {
                const auto contains=[&](std::uint32_t pc) { return pc>=range.address && std::uint64_t(pc)<std::uint64_t(range.address)+range.size; };
                for (unsigned k=1;k<4;++k) same_guard&=contains(i.pc)==contains(instructions[n+k].pc);
            }
            for (const auto& payload:payloads) {
                const auto contains=[&](std::uint32_t pc) { return physical_address(pc)>=payload.address && std::uint64_t(physical_address(pc))<std::uint64_t(payload.address)+payload.bytes.size(); };
                for (unsigned k=1;k<4;++k) same_guard&=contains(i.pc)==contains(instructions[n+k].pc);
            }
            if (same_guard) {
                const auto enough=w.value("icmp uge i64 "+remaining+", 4");
                w.out<<"  br i1 "<<enough<<", label %display_wait"<<suffix<<", label %display_continue"<<suffix<<"\ndisplay_wait"<<suffix<<":\n";
                const auto address=w.binary("add","i32",w.narrow(w.reg(i.rs)),std::to_string(static_cast<std::uint32_t>(static_cast<std::int32_t>(i.immediate))));
                auto eligible=std::string("true");
                const auto exclude=[&](std::uint32_t base,std::uint32_t bytes) {
                    const auto relative=w.binary("sub","i32",address,std::to_string(base));
                    eligible=w.binary("and","i1",eligible,w.value("icmp uge i32 "+relative+", "+std::to_string(bytes)));
                };
                for (const auto& view:layout.aliases) exclude(view.address,view.bytes);
                for (const auto& guard:layout.guards) if (!guard.readable) exclude(guard.address,guard.bytes);
                const auto hint=eligible=="true" ? address : w.value("select i1 "+eligible+", i32 "+address+", i32 0");
                const auto result=w.value("call i32 @nt_ee_wait_display(ptr %memory, i32 "+hint+")");
                const auto ready=w.value("icmp eq i32 "+result+", 0");
                w.out<<"  br i1 "<<ready<<", label %display_continue"<<suffix<<", label %display_stop"<<suffix
                     <<"\ndisplay_stop"<<suffix<<":\n  ret i32 "<<result<<"\ndisplay_continue"<<suffix<<":\n";
            }
        }
        if (native_layout_transaction(instructions.subspan(n)) && !(n && instructions[n-1].branch())) {
            const auto enough=w.value("icmp uge i64 "+remaining+", 7");
            w.out<<"  br i1 "<<enough<<", label %layout_call"<<suffix<<", label %budget_stop\nlayout_call"<<suffix<<":\n";
            std::string args;
            std::array<std::string,5> values;
            for (unsigned field=0;field<5;++field) {
                values[field]=w.narrow(w.reg(instructions[n+field].rt));
                args+=", i32 "+values[field];
            }
            std::string planned="false";
            for (const auto& declaration:layout.declarations) {
                std::string matches="true";
                for (unsigned field=0;field<4;++field)
                    matches=w.binary("and","i1",matches,w.value("icmp eq i32 "+values[field+1]+", "+std::to_string(declaration[field])));
                planned=w.binary("or","i1",planned,matches);
            }
            // Clearing an unallocated high-address view has no native resource
            // to release. Prove it is disjoint from every compiled allocation.
            const auto begin=w.value("zext i32 "+values[2]+" to i64");
            const auto bytes=w.binary("add","i64",w.value("zext i32 "+values[1]+" to i64"),"8192");
            const auto end=w.binary("add","i64",begin,bytes);
            auto vacant=w.binary("and","i1",w.value("icmp eq i32 "+values[3]+", 0"),
                w.value("icmp eq i32 "+values[4]+", 0"));
            vacant=w.binary("and","i1",vacant,w.value("icmp uge i32 "+values[2]+", 3221225472"));
            vacant=w.binary("and","i1",vacant,w.value("icmp ule i64 "+end+", 4294967296"));
            for (const auto& view:layout.aliases) {
                const auto before=w.value("icmp ule i64 "+end+", "+std::to_string(view.address));
                const auto after=w.value("icmp uge i64 "+begin+", "+std::to_string(std::uint64_t(view.address)+view.bytes));
                vacant=w.binary("and","i1",vacant,w.binary("or","i1",before,after));
            }
            planned=w.binary("or","i1",planned,vacant);
            args+=", i32 "+w.value("zext i1 "+planned+" to i32");
            const auto result=w.value("call i32 @nt_ee_validate_native_layout(ptr %memory"+args+")");
            const auto layout_ready=w.value("icmp eq i32 "+result+", 0");
            w.out<<"  br i1 "<<layout_ready<<", label %layout_done"<<suffix<<", label %layout_stop"<<suffix
                 <<"\nlayout_stop"<<suffix<<":\n  ret i32 "<<result<<"\nlayout_done"<<suffix<<":\n";
            w.store("i32",std::to_string(i.pc+28),offsetof(runtime::State,pc));
            w.store("i64",w.binary("add","i64",w.load("i64",offsetof(runtime::State,executed)),"7"),offsetof(runtime::State,executed));
            const auto left=w.binary("sub","i64",remaining,"7");
            w.out<<"  store i64 "<<left<<", ptr %remaining\n  br label %dispatch\n";
            continue;
        }
        if (n+1<instructions.size()) if (const auto pattern=constant_pattern(i,instructions[n+1])) {
            const auto next_pc=instructions[n+1].pc;
            const auto contains=[](std::uint32_t pc,std::uint32_t address,std::uint64_t size) {
                return pc>=address && std::uint64_t(pc)<std::uint64_t(address)+size;
            };
            bool same_guard=true;
            for (const auto& range:ranges)
                same_guard&=contains(i.pc,range.address,range.size)==contains(next_pc,range.address,range.size);
            for (const auto& payload:payloads)
                same_guard&=contains(physical_address(i.pc),payload.address,payload.bytes.size())==
                    contains(physical_address(next_pc),payload.address,payload.bytes.size());
            if (same_guard) {
                const auto enough=w.value("icmp uge i64 "+remaining+", 2");
                const auto fast=w.binary("and","i1",enough,w.binary("xor","i1",is_delay,"true"));
                w.out<<"  br i1 "<<fast<<", label %constant_pair"<<suffix<<", label %constant_single"<<suffix
                     <<"\nconstant_pair"<<suffix<<":\n";
                if (pattern->destination) w.store("i64",std::to_string(pattern->value),
                    offsetof(runtime::State,gpr)+pattern->destination*sizeof(runtime::Register));
                w.store("i32",std::to_string(i.pc+8),offsetof(runtime::State,pc));
                w.store("i64",w.binary("add","i64",w.load("i64",offsetof(runtime::State,executed)),"2"),offsetof(runtime::State,executed));
                const auto left=w.binary("sub","i64",remaining,"2");
                w.out<<"  store i64 "<<left<<", ptr %remaining\n";
                w.out<<"  br label "<<(n+2<instructions.size() && instructions[n+2].pc==i.pc+8 ?
                    "%block"+std::to_string(i.pc+8) : "%unmapped")<<"\nconstant_single"<<suffix<<":\n";
            }
        }
        if (i.op==Op::breakpoint) { w.out<<"  ret i32 11\n"; continue; }
        if (i.op==Op::syscall) { w.out<<"  ret i32 1\n"; continue; }
        std::string taken="true";
        if (i.branch()) {
            taken=branch_condition(w,i);
            // Capture operands before writing the link or executing the delay slot.
            std::string target;
            if (i.op==Op::jr || i.op==Op::jalr) target=w.narrow(w.reg(i.rs));
            else target=w.value("select i1 "+taken+", i32 "+std::to_string(i.target())+", i32 "+std::to_string(i.pc+8));
            const bool conditional_link=i.op==Op::bltzal || i.op==Op::bgezal || i.op==Op::bltzall || i.op==Op::bgezall;
            if (conditional_link || i.op==Op::jal || (i.op==Op::jalr && i.rd)) {
                const auto reg=conditional_link || i.op==Op::jal ? 31U : i.rd;
                w.store("i64",w.extend(std::to_string(i.pc+8)),offsetof(runtime::State,gpr)+reg*sizeof(runtime::Register));
            }
            w.store("i32",target,offsetof(runtime::State,pending_target));
            w.store("i32",suffix,offsetof(runtime::State,branch_pc));
            w.store("i8",i.likely() ? w.value("zext i1 "+taken+" to i8") : "1",offsetof(runtime::State,delay_slot));
            const auto successor=i.likely() ? w.value("select i1 "+taken+", i32 "+std::to_string(i.pc+4)+", i32 "+std::to_string(i.pc+8)) : std::to_string(i.pc+4);
            w.store("i32",successor,offsetof(runtime::State,pc));
        } else if (i.op==Op::eret) {
            const auto error=w.value("icmp ne i32 "+w.binary("and","i32",status,"4")+", 0");
            const auto epc=w.load("i32",offsetof(runtime::State,cop0_epc));
            const auto error_epc=w.load("i32",offsetof(runtime::State,cop0_error_epc));
            w.store("i32",w.value("select i1 "+error+", i32 "+error_epc+", i32 "+epc),offsetof(runtime::State,pc));
            const auto mask=w.value("select i1 "+error+", i32 4294967291, i32 4294967293");
            w.store("i32",w.binary("and","i32",status,mask),offsetof(runtime::State,cop0_status));
            w.store("i8","0",offsetof(runtime::State,delay_slot));
        } else {
            if (const auto access=memory_operation(i.op)) emit_memory(w,i,*access,ranges,layout.aliases,layout.guards);
            else if (address_space_operation(i.op)) emit_address_space(w,i);
            else if (i.op==Op::mfc0 || i.op==Op::mtc0) emit_system_transfer(w,i,layout.declarations.size());
            else if (const auto packed=packed_operation(i.op)) emit_packed(w,i,*packed);
            else if (const auto multiply=integer_multiply(i.op)) emit_integer_multiply(w,i,*multiply);
            else if (packed_accumulator_transfer(i.op)) emit_packed_accumulator_transfer(w,i);
            else if (vector_operation(i.op)) emit_vector(w,i);
            else if (const auto predicate=trap_predicate(i.op)) emit_trap(w,i,predicate);
            else if (floating_operation(i.op)) emit_floating(w,i);
            else if (halfword_product(i.op)) emit_halfword_product(w,i);
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
        if (i.op==Op::eret) w.out<<"  br label %dispatch\n";
        else if (i.likely()) w.out<<"  br i1 "<<taken<<", label "<<sequential<<", label %dispatch\n";
        else if (!i.branch() && n && instructions[n-1].pc+4==i.pc && instructions[n-1].branch())
            w.out<<"  br i1 "<<is_delay<<", label %dispatch, label "<<sequential<<"\n";
        else w.out<<"  br label "<<sequential<<"\n";
    }
    w.out<<"interrupt_stop:\n  ret i32 7\nbudget_stop:\n  ret i32 0\nunmapped:\n  ret i32 2\ninvalid:\n  ret i32 3\n}\n";
}

}
namespace {
void add_constraint(EeRejection& rejection,std::string message) {
    if (rejection.constraints.size()<32) rejection.constraints.push_back(std::move(message));
    else ++rejection.omitted_constraints;
}
}
std::string EeRejection::text() const {
    std::ostringstream diagnostic;
    if (!families.empty()) {
        diagnostic<<"EE LLVM unsupported operation families ("<<families.size()<<"):\n";
        for (const auto& family:families)
            diagnostic<<"  "<<family.name<<": "<<family.count<<" occurrence(s), first at 0x"<<std::hex<<family.first<<std::dec<<'\n';
    }
    if (!constraints.empty() || omitted_constraints) {
        diagnostic<<"EE LLVM encoding constraints ("<<(constraints.size()+omitted_constraints)<<"):\n";
        for (const auto& constraint:constraints) diagnostic<<"  "<<constraint<<'\n';
        if (omitted_constraints) diagnostic<<"  "<<omitted_constraints<<" additional encoding constraint(s) omitted\n";
    }
    return diagnostic.str();
}
EeRejection reject_unsupported(std::span<const r5900::Instruction> instructions) {
    EeRejection rejection;
    std::map<std::string,std::size_t> index;
    for (std::size_t n=0;n<instructions.size();++n) {
        const auto& i=instructions[n];
        if (!supported(i.op)) {
            std::ostringstream family;
            family<<r5900::name(i.op);
            if (i.op==r5900::Op::unsupported) {
                const auto primary=i.word>>26;
                family<<" (primary="<<primary;
                if (primary==0 || primary==28 || primary==17 || primary==18) family<<", function="<<(i.word&63);
                if (primary==28) family<<", subgroup="<<i.sa;
                if (primary==16 || primary==17 || primary==18) family<<", format="<<i.rs;
                family<<")";
            }
            const auto key=family.str();
            const auto found=index.find(key);
            if (found==index.end()) {
                index.emplace(key,rejection.families.size());
                rejection.families.push_back({key,i.pc,1});
            } else ++rejection.families[found->second].count;
        }
        std::ostringstream where;
        where<<"0x"<<std::hex<<i.pc;
        if ((i.op==r5900::Op::mfc0 || i.op==r5900::Op::mtc0) && i.rd!=0 && i.rd!=2 && i.rd!=3 && i.rd!=5 && i.rd!=10 && i.rd!=25 && i.rd!=12 && i.rd!=14 && i.rd!=30 && i.rd!=6 && !((i.rd==28 || i.rd==9 || i.rd==1) && i.op==r5900::Op::mfc0))
            add_constraint(rejection,"EE system transfer selector is not supported at "+where.str());
        if (i.op==r5900::Op::cfc2 && i.rd==20)
            add_constraint(rejection,"EE vector random-register transfer is not implemented at "+where.str());
        if ((i.op==r5900::Op::ctc1 || i.op==r5900::Op::cfc1) && i.rd!=31)
            add_constraint(rejection,"EE floating control transfer supports FCR31 only at "+where.str());
        if (i.op==r5900::Op::sync && (i.rs || i.rt || i.rd || (i.sa!=0 && i.sa!=16)))
            add_constraint(rejection,"Unsupported EE SYNC encoding at "+where.str());
        if (i.branch() && (n+1==instructions.size() || instructions[n+1].pc!=i.pc+4 || instructions[n+1].branch() || instructions[n+1].op==r5900::Op::syscall || instructions[n+1].op==r5900::Op::eret))
            add_constraint(rejection,"EE branch requires a supported scalar delay slot at "+where.str());
    }
    return rejection;
}
void emit_declarations(Writer& w) {
    w.out<<"declare i32 @memcmp(ptr, ptr, i64)\n";
    w.out<<"declare i32 @nt_ee_validate_native_layout(ptr, i32, i32, i32, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_address_space(ptr, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_performance(ptr, i32, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_test_completions(ptr)\n";
    w.out<<"declare i32 @nt_ee_wait_display(ptr, i32)\n";
    w.out<<"declare i64 @nt_ee_io_read32(ptr, i32)\ndeclare i32 @nt_ee_io_write32(ptr, i32, i32)\ndeclare i32 @nt_ee_io_write8(ptr, i32, i32)\n";
    w.out<<"declare i32 @nt_ee_io_read64(ptr, ptr, i32, i32)\ndeclare i32 @nt_ee_io_write64(ptr, i32, i64)\n";
    w.out<<"declare i32 @nt_ee_io_write128(ptr, i32, i64, i64)\n";
    declare_packed_intrinsics(w.out);
    w.out<<"declare i64 @nt_ee_square_root_finite(i32)\n"
         <<"declare i64 @nt_ee_reciprocal_square_root_finite(i32, i32)\n"
         <<"declare i64 @nt_ee_multiply_accumulate_finite(i32, i32, i32, i32)\n"
         <<"declare i64 @nt_ee_scalar_multiply_accumulate(i32, i32, i32, i32, i32)\n";
    w.out<<"declare i32 @nt_ee_vector_multiply_accumulate(ptr, i32, i32, i32, i32, i32, i32)\n"
         <<"declare i32 @nt_ee_vector_quotient(ptr, i32, i32, i32, i32, i32)\n"
         <<"declare i32 @nt_ee_vector_outer(ptr, i32, i32, i32, i32)\n"
         <<"declare i32 @nt_vu_clip_flags(i32, i32, i32, i32)\n"
         <<"declare i32 @nt_ee_vector_multiply(ptr, i32, i32, i32, i32, i32)\n"
         <<"declare i32 @nt_ee_integer_to_scaled_scalar(i32, i32)\n"
         <<"declare i32 @nt_ee_scaled_scalar_to_integer(i32, i32)\n";
    w.out<<"declare i64 @nt_ee_scalar_add(i32, i32, i32)\n"
         <<"declare i64 @nt_ee_scalar_multiply(i32, i32)\n";
    w.out<<"declare i64 @nt_ee_vector_read(ptr, i32, i32)\n";
    w.out<<"declare i32 @nt_ee_vector_launch(ptr, i32, i32)\n";
    w.out<<"declare i32 @nt_ee_vector_memory(ptr, i32)\n";
    w.out<<"declare i64 @nt_ee_vector_write(ptr, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_multiply_finite(i32, i32)\n";
    w.out<<"declare i64 @nt_ee_divide_finite(i32, i32)\n"
         <<"declare i32 @nt_ee_integer_to_scalar(i32)\n"
         <<"declare i32 @nt_ee_scalar_to_integer(i32)\n"
         <<"declare i32 @nt_ee_min_max_scalar(i32, i32, i32)\n";

    w.out<<"declare i32 @nt_ee_vector_add(ptr, i32, i32, i32, i32, i32, i32)\n";
    w.out<<"declare <2 x i32> @llvm.ctlz.v2i32(<2 x i32>, i1 immarg)\n";
    for (const auto width : {32,64}) for (const auto operation : {"sadd","ssub"})
        w.out<<"declare { i"<<width<<", i1 } @llvm."<<operation<<".with.overflow.i"<<width<<"(i"<<width<<", i"<<width<<")\n";
}
void emit_payloads(Writer& w,std::span<const CodePayload> payloads) {
    for (std::size_t n=0;n<payloads.size();++n) {
        w.out<<"@nt_payload_"<<n<<" = private constant ["<<payloads[n].bytes.size()<<" x i8] [";
        for (std::size_t b=0;b<payloads[n].bytes.size();++b)
            w.out<<(b ? ", " : "")<<"i8 "<<static_cast<unsigned>(payloads[n].bytes[b]);
        w.out<<"]\n";
    }
}
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> protected_code,std::span<const CodePayload> payloads,std::span<const LayoutDeclaration> layouts,std::span<const ExecutableImage> images,
               const std::function<std::ostream&(std::size_t)>& module_output,std::uint64_t input_identity) {
    static_assert(std::is_standard_layout_v<runtime::State> && std::is_standard_layout_v<ee::Memory>);
    static_assert(sizeof(void*)==8 && std::endian::native==std::endian::little,
                  "EE LLVM currently requires a little-endian 64-bit host ABI");
    if (instructions.empty() || instructions.size()>program_instruction_limit) throw std::invalid_argument("EE program exceeds the bounded instruction limit or is empty");
    for (std::size_t n=0;n<payloads.size();++n) {
        const auto& payload=payloads[n];
        const auto end=std::uint64_t(payload.address)+payload.bytes.size();
        if (payload.address%4 || payload.bytes.empty() || payload.bytes.size()%4 || end>0x2000000U)
            throw std::invalid_argument("Invalid copied-code payload extent");
        for (std::size_t earlier=0;earlier<n;++earlier)
            if (payload.address<std::uint64_t(payloads[earlier].address)+payloads[earlier].bytes.size() && payloads[earlier].address<end)
                throw std::invalid_argument("Copied-code payload extents overlap");
        for (const auto& range:protected_code)
            if (payload.address<std::uint64_t(range.address)+range.size && range.address<end)
                throw std::invalid_argument("Copied-code payload overlaps protected ELF code");
    }
    for (std::size_t n=0;n<instructions.size();++n) {
        const auto& i=instructions[n];
        const auto decoded=r5900::decode(i.pc,i.word);
        if (i.pc%4 || (n && i.pc<=instructions[n-1].pc) ||
            i.op!=decoded.op || i.rs!=decoded.rs || i.rt!=decoded.rt || i.rd!=decoded.rd || i.sa!=decoded.sa || i.immediate!=decoded.immediate)
            throw std::invalid_argument("EE region contains inconsistent decoded instructions");
    }
    const auto rejection=reject_unsupported(instructions);
    // Unsupported opcodes stay a conversion diagnostic. Encoding mistakes stay
    // argument errors so callers can distinguish a bad instruction from a
    // missing operation family. A family diagnostic still includes constraints.
    if (!rejection.families.empty()) throw std::runtime_error(rejection.text());
    if (!rejection.constraints.empty() || rejection.omitted_constraints)
        throw std::invalid_argument(rejection.constraints.empty() ? std::string("EE LLVM encoding constraints") : rejection.constraints.front());
    validate_images(images,protected_code,instructions);
    std::vector<CodeRange> ranges(protected_code.begin(),protected_code.end());
    for (const auto& range:ranges)
        if (!range.size || std::uint64_t(range.address)+range.size>0x100000000ULL)
            throw std::invalid_argument("Invalid EE executable protection extent");
    for (const auto& i:instructions) {
        const auto address=physical_address(i.pc);
        const auto payload=std::find_if(payloads.begin(),payloads.end(),[&](const auto& p) {
            return address>=p.address && std::uint64_t(address)<std::uint64_t(p.address)+p.bytes.size();
        });
        if (payload!=payloads.end()) {
            const auto offset=address-payload->address;
            const auto word=std::uint32_t(payload->bytes[offset]) | (std::uint32_t(payload->bytes[offset+1])<<8) |
                (std::uint32_t(payload->bytes[offset+2])<<16) | (std::uint32_t(payload->bytes[offset+3])<<24);
            if (word!=i.word) throw std::invalid_argument("Compiled payload instruction differs from expected bytes");
            continue;
        }
        if (std::any_of(ranges.begin(),ranges.end(),[&](const auto& r) {
            return address>=r.address && std::uint64_t(address)+4<=std::uint64_t(r.address)+r.size;
        })) continue;
        if (!ranges.empty() && !ranges.back().image && std::uint64_t(ranges.back().address)+ranges.back().size==address)
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
            if (range.address<previous_end && range.image!=previous.image)
                throw std::invalid_argument("Native executable lifetimes overlap");
            if (range.image==previous.image && range.address<=previous_end && end-previous.address<=0xffffffffULL) {
                previous.size=static_cast<std::uint32_t>(end-previous.address); continue;
            }
        }
        merged.push_back(range);
    }
    ranges=std::move(merged);
    std::vector<std::span<const r5900::Instruction>> parts;
    for (std::size_t first=0;first<instructions.size();) {
        const auto unit=instructions[first].pc/module_address_span;
        const auto boundary=(std::uint64_t(unit)+1)*module_address_span;
        const auto finish=std::lower_bound(instructions.begin()+first,instructions.end(),boundary,
            [](const auto& instruction,std::uint64_t end) { return instruction.pc<end; });
        auto count=std::min(function_instruction_limit,static_cast<std::size_t>(finish-(instructions.begin()+first)));
        // Never separate a branch from its delay slot. A branch in the final
        // word of a unit may own one word across the nominal unit boundary.
        // reject_unsupported() has already checked the adjacent scalar slot.
        if (instructions[first+count-1].branch()) count=count==1 ? 2 : count-1;
        parts.push_back(instructions.subspan(first,count)); first+=count;
    }
    const auto layout=prepare_native_layout(layouts);
    Writer w{output};
    w.out<<"define i64 @nt_ee_generated_input_identity() {\nentry:\n  ret i64 "<<input_identity<<"\n}\n";
    emit_images(w,images);
    if (module_output) {
        for (std::size_t first=0;first<parts.size();) {
            const auto unit_id=parts[first].front().pc/module_address_span;
            auto end=first+1;
            while (end<parts.size() && parts[end].front().pc/module_address_span==unit_id) ++end;
            auto& stream=module_output(unit_id);
            Writer unit{stream};
            // Keep code protection authoritative in the common module. A new
            // compiled range must not duplicate guard changes in every object.
            unit.shared_code_guard=true;
            emit_declarations(unit);
            unit.out<<"declare i1 @nt_ee_check_code_range(i64, i64, i32)\n";
            emit_payloads(unit,payloads);
            for (auto index=first;index<end;++index) {
                emit_part(unit,parts[index],ranges,payloads,layout,true);
                w.out<<"declare i32 @nt_ee_part_"<<parts[index].front().pc<<"(ptr, ptr, i64)\n";
            }
            if (!stream) throw std::runtime_error("Cannot finish EE LLVM module");
            first=end;
        }
    } else {
        emit_payloads(w,payloads);
        for (const auto part:parts) emit_part(w,part,ranges,payloads,layout);
    }
    emit_dispatcher(w,parts);
    emit_contains(w,instructions);
    emit_code_protection(w,ranges);

    w.out<<"define i1 @nt_ee_generated_code_overlap(i32 %address, i32 %bytes) {\nentry:\n"
         <<"  %result = call i1 @nt_ee_generated_active_code_overlap(i32 %address, i32 %bytes, i32 1)\n  ret i1 %result\n}\n";
    w.out<<"define i1 @nt_ee_generated_active_code_overlap(i32 %address, i32 %bytes, i32 %image) {\nentry:\n";
    const auto address=w.value("zext i32 %address to i64");
    const auto bytes=w.value("zext i32 %bytes to i64");
    const auto end=w.binary("add","i64",address,bytes);
    const auto overlap=w.value("call i1 @nt_ee_check_code_range(i64 "+address+", i64 "+end+", i32 %image)");
    w.out<<"  ret i1 "<<overlap<<"\n}\n";
    emit_declarations(w);
    if (!output) throw std::runtime_error("Cannot finish EE LLVM output");
}
}
