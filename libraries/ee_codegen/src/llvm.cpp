#include <nt/ee_codegen.hpp>
#include "llvm_writer.hpp"
#include "memory.hpp"
#include "packed.hpp"
#include "vector.hpp"
#include "floating.hpp"
#include "integer_multiply.hpp"
#include "packed_multiply.hpp"
#include "dispatch.hpp"
#include "branches.hpp"
#include "traps.hpp"
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

void emit_part(Writer& w,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> ranges,std::size_t index,std::span<const CodePayload> payloads) {
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
        if (native_layout_transaction(instructions.subspan(n)) && !(n && instructions[n-1].branch())) {
            const auto enough=w.value("icmp uge i64 "+remaining+", 7");
            w.out<<"  br i1 "<<enough<<", label %layout_call"<<suffix<<", label %budget_stop\nlayout_call"<<suffix<<":\n";
            std::string args;
            for (unsigned field=0;field<5;++field) args+=", i32 "+w.narrow(w.reg(instructions[n+field].rt));
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
            if (const auto access=memory_operation(i.op)) emit_memory(w,i,*access,ranges);
            else if (address_space_operation(i.op)) emit_address_space(w,i);
            else if (i.op==Op::mfc0 || i.op==Op::mtc0) emit_system_transfer(w,i);
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
        if ((i.op==r5900::Op::mfc0 || i.op==r5900::Op::mtc0) && i.rd!=0 && i.rd!=2 && i.rd!=3 && i.rd!=5 && i.rd!=10 && i.rd!=25 && i.rd!=12 && i.rd!=14 && i.rd!=30 && i.rd!=6 && !(i.rd==28 && i.op==r5900::Op::mfc0))
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
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,std::span<const CodeRange> protected_code,std::span<const CodePayload> payloads) {
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
    if (!payloads.empty()) w.out<<"declare i32 @memcmp(ptr, ptr, i64)\n";
    for (std::size_t n=0;n<payloads.size();++n) {
        w.out<<"@nt_payload_"<<n<<" = private constant ["<<payloads[n].bytes.size()<<" x i8] [";
        for (std::size_t b=0;b<payloads[n].bytes.size();++b)
            w.out<<(b ? ", " : "")<<"i8 "<<static_cast<unsigned>(payloads[n].bytes[b]);
        w.out<<"]\n";
    }
    for (std::size_t index=0;index<parts.size();++index) emit_part(w,parts[index],ranges,index,payloads);
    emit_dispatcher(w,parts);
    w.out<<"define i1 @nt_ee_generated_contains(i32 %address) {\nentry:\n  switch i32 %address, label %missing [\n";
    for (const auto& i:instructions) w.out<<"    i32 "<<i.pc<<", label %present\n";
    w.out<<"  ]\npresent:\n  ret i1 true\nmissing:\n  ret i1 false\n}\n";

    w.out<<"define i1 @nt_ee_generated_code_overlap(i32 %address, i32 %bytes) {\nentry:\n";
    const auto address=w.value("zext i32 %address to i64");
    const auto bytes=w.value("zext i32 %bytes to i64");
    const auto end=w.binary("add","i64",address,bytes);
    const auto overlap=code_overlap(w,address,end,ranges);
    w.out<<"  ret i1 "<<overlap<<"\n}\n";
    w.out<<"declare i32 @nt_ee_validate_native_layout(ptr, i32, i32, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_address_space(ptr, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_performance(ptr, i32, i32, i32, i32)\n";
    w.out<<"declare i64 @nt_ee_io_read32(ptr, i32)\ndeclare i32 @nt_ee_io_write32(ptr, i32, i32)\ndeclare i32 @nt_ee_io_write8(ptr, i32, i32)\n";
    w.out<<"declare i32 @nt_ee_io_read64(ptr, ptr, i32, i32)\ndeclare i32 @nt_ee_io_write64(ptr, i32, i64)\n";
    declare_packed_intrinsics(w.out);
    w.out<<"declare i64 @nt_ee_square_root_finite(i32)\n";
    w.out<<"declare i32 @nt_ee_vector_multiply(ptr, i32, i32, i32, i32, i32)\n"
         <<"declare i32 @nt_ee_integer_to_scaled_scalar(i32, i32)\n"
         <<"declare i32 @nt_ee_scaled_scalar_to_integer(i32, i32)\n";
    w.out<<"declare i64 @nt_ee_add_finite(i32, i32, i32)\n";
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
    output<<w.out.str();
}
}
