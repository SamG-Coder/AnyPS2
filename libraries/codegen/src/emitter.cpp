#include <nt/codegen.hpp>
#include <sstream>
#include <stdexcept>

namespace nt::codegen {
namespace {
std::string reg(unsigned n) { return "s.gpr[" + std::to_string(n) + "].lo"; }
void operation(std::ostream& o, const r5900::Instruction& i) {
    using r5900::Op;
    const auto rs = reg(i.rs), rt = reg(i.rt);
    const auto imm = "static_cast<std::uint64_t>(std::int64_t(" + std::to_string(i.immediate) + "))";
    const auto uimm = std::to_string(i.word & 65535) + "ULL";
    const auto set = [&](unsigned r, const std::string& expr) { o << "s.set(" << r << ", " << expr << ");\n"; };
    const auto branch = [&](const std::string& condition, const std::string& target) {
        o << "const bool taken = " << condition << ";\n";
        if (i.likely()) o << "if (!taken) { next = " << hex32(i.pc + 8) << "U; } else ";
        o << "{ new_delay = true; after = taken ? " << target << " : " << hex32(i.pc + 8) << "U; }\n";
    };
    const auto shift = std::to_string(i.sa);
    const auto address = "static_cast<std::uint32_t>(" + rs + " + " + imm + ")";
    switch (i.op) {
    case Op::cache: o << "bus.synchronize_cache(" << (i.rt == 0x1a ? 2 : 0) << ");\n"; break;
    case Op::div: case Op::divu:
        o << "divide_words(s,std::uint32_t(" << rs << "),std::uint32_t(" << rt << "),"
          << (i.op == Op::div ? "true" : "false") << ");\n"; break;
    case Op::mult: case Op::multu:
        if (i.op == Op::mult)
            o << "const auto product = std::uint64_t(std::int64_t(std::bit_cast<std::int32_t>(std::uint32_t(" << rs
              << "))) * std::int64_t(std::bit_cast<std::int32_t>(std::uint32_t(" << rt << "))));\n";
        else o << "const auto product = std::uint64_t(std::uint32_t(" << rs << ")) * std::uint32_t(" << rt << ");\n";
        o << "s.lo = sx32(std::uint32_t(product)); s.hi = sx32(std::uint32_t(product >> 32));\n";
        set(i.rd,"s.lo"); break;
    case Op::sdl: case Op::sdr:
        o << "const auto address = " << address << ";\n"
          << "const auto old = bus.load(address & ~7U,8);\n"
          << "bus.store(address & ~7U,8,merge_double_store(old," << rt << ",address & 7U,"
          << (i.op == Op::sdl ? "true" : "false") << "));\n"; break;
    case Op::ldl: case Op::ldr:
        o << "const auto address = " << address << ";\n";
        set(i.rt,"merge_double_load(" + rt + ", bus.load(address & ~7U,8), address & 7U, " +
            (i.op == Op::ldl ? "true" : "false") + ")"); break;
    case Op::plzcw: set(i.rd,"plzcw(" + rs + ")"); break;
    case Op::ei: case Op::di:
        o << "if ((s.cop0_status & 0x20006U) || !(s.cop0_status & 0x18U)) s.cop0_status "
          << (i.op == Op::ei ? "|= 0x10000U;\n" : "&= ~0x10000U;\n"); break;
    case Op::mfc0: set(i.rt,i.rd == 12 ? "sx32(s.cop0_status & 0xf0c79c1fU)" : i.rd == 14 ? "sx32(s.cop0_epc)" : "sx32(s.cop0_error_epc)"); break;
    case Op::mtc0: o << (i.rd == 12 ? "s.cop0_status" : i.rd == 14 ? "s.cop0_epc" : "s.cop0_error_epc") << " = std::uint32_t(" << rt << ");\n"; break;
    case Op::eret:
        o << "if (was_delay) return Result{Stop::branch_in_delay,s.pc,word};\n"
             "if (s.cop0_status & 4) { next = s.cop0_error_epc; s.cop0_status &= ~4U; }\n"
             "else { next = s.cop0_epc; s.cop0_status &= ~2U; }\n"; break;
    case Op::pcpyh:
        o << "s.set128(" << i.rd << ", {(s.gpr[" << i.rt << "].lo & 65535) * 0x0001000100010001ULL, (s.gpr[" << i.rt << "].hi & 65535) * 0x0001000100010001ULL});\n"; break;
    case Op::pcpyld:
        o << "s.set128(" << i.rd << ", {s.gpr[" << i.rt << "].lo, s.gpr[" << i.rs << "].lo});\n"; break;
    case Op::pcpyud:
        o << "s.set128(" << i.rd << ", {s.gpr[" << i.rs << "].hi, s.gpr[" << i.rt << "].hi});\n"; break;
    case Op::sll: set(i.rd, "sx32(std::uint32_t(" + rt + ") << " + shift + ")"); break;
    case Op::srl: set(i.rd, "sx32(std::uint32_t(" + rt + ") >> " + shift + ")"); break;
    case Op::sra: set(i.rd, "sx32(std::uint32_t(std::bit_cast<std::int32_t>(std::uint32_t(" + rt + ")) >> " + shift + "))"); break;
    case Op::sllv: set(i.rd, "sx32(std::uint32_t(" + rt + ") << (" + rs + " & 31))"); break;
    case Op::srlv: set(i.rd, "sx32(std::uint32_t(" + rt + ") >> (" + rs + " & 31))"); break;
    case Op::srav: set(i.rd, "sx32(std::uint32_t(std::bit_cast<std::int32_t>(std::uint32_t(" + rt + ")) >> (" + rs + " & 31)))"); break;
    case Op::addiu: set(i.rt, "sx32(std::uint32_t(" + rs + ") + std::uint32_t(" + imm + "))"); break;
    case Op::daddiu: set(i.rt, rs + " + " + imm); break;
    case Op::addu: set(i.rd, "sx32(std::uint32_t(" + rs + ") + std::uint32_t(" + rt + "))"); break;
    case Op::subu: set(i.rd, "sx32(std::uint32_t(" + rs + ") - std::uint32_t(" + rt + "))"); break;
    case Op::daddu: set(i.rd, rs + " + " + rt); break;
    case Op::dsubu: set(i.rd, rs + " - " + rt); break;
    case Op::lui: set(i.rt, "sx32(" + hex32((i.word & 65535) << 16) + "U)"); break;
    case Op::andi: set(i.rt, rs + " & " + uimm); break;
    case Op::ori: set(i.rt, rs + " | " + uimm); break;
    case Op::xori: set(i.rt, rs + " ^ " + uimm); break;
    case Op::bit_and: set(i.rd, rs + " & " + rt); break;
    case Op::bit_or: set(i.rd, rs + " | " + rt); break;
    case Op::bit_xor: set(i.rd, rs + " ^ " + rt); break;
    case Op::bit_nor: set(i.rd, "~(" + rs + " | " + rt + ")"); break;
    case Op::slt: set(i.rd, "signed64(" + rs + ") < signed64(" + rt + ")"); break;
    case Op::sltu: set(i.rd, rs + " < " + rt); break;
    case Op::slti: set(i.rt, "signed64(" + rs + ") < " + std::to_string(i.immediate)); break;
    case Op::sltiu: set(i.rt, rs + " < " + imm); break;
    case Op::dsll: case Op::dsll32: set(i.rd, rt + " << " + std::to_string(i.sa + (i.op == Op::dsll32 ? 32 : 0))); break;
    case Op::dsrl: case Op::dsrl32: set(i.rd, rt + " >> " + std::to_string(i.sa + (i.op == Op::dsrl32 ? 32 : 0))); break;
    case Op::dsra: case Op::dsra32: set(i.rd, "std::uint64_t(signed64(" + rt + ") >> " + std::to_string(i.sa + (i.op == Op::dsra32 ? 32 : 0)) + ")"); break;
    case Op::movz: case Op::movn:
        o << "if (" << rt << (i.op == Op::movz ? " == 0) " : " != 0) "); set(i.rd, rs); break;
    case Op::mfhi: set(i.rd, "s.hi"); break;
    case Op::mflo: set(i.rd, "s.lo"); break;
    case Op::mthi: o << "s.hi = " << rs << ";\n"; break;
    case Op::mtlo: o << "s.lo = " << rs << ";\n"; break;
    case Op::mfhi1: set(i.rd, "s.hi1"); break;
    case Op::mflo1: set(i.rd, "s.lo1"); break;
    case Op::mthi1: o << "s.hi1 = " << rs << ";\n"; break;
    case Op::mtlo1: o << "s.lo1 = " << rs << ";\n"; break;
    case Op::mfsa: set(i.rd, "s.sa"); break;
    case Op::mtsa: o << "s.sa = std::uint32_t(" << rs << ");\n"; break;
    case Op::mtsab: o << "s.sa = (" << rs << " & 15) ^ " << (i.word & 15) << ";\n"; break;
    case Op::mtsah: o << "s.sa = ((" << rs << " & 7) ^ " << (i.word & 7) << ") << 1;\n"; break;
    case Op::mtc1: o << "s.fpr[" << i.rd << "] = std::uint32_t(" << rt << ");\n"; break;
    case Op::mfc1: set(i.rt, "sx32(s.fpr[" + std::to_string(i.rd) + "])"); break;
    case Op::ctc1:
        if (i.rd == 31) o << "s.fcr31 = std::uint32_t(" << rt << ");\n";
        break;
    case Op::cfc1: set(i.rt, i.rd == 31 ? "sx32(s.fcr31)" : i.rd == 0 ? "0x2e00U" : "0U"); break;
    case Op::adda_s:
        // Exact guarded specialization, not host IEEE floating point. All other
        // operands stop until EE floating point behavior is implemented/validated.
        o << "if (s.fpr[" << i.rd << "] != 0 || s.fpr[" << i.rt << "] != 0) return Result{Stop::unsupported, s.pc, word};\n"
             "s.accumulator = 0; s.fcr31 &= ~0xc000U;\n";
        break;
    case Op::j: case Op::jal:
        branch("true", hex32(i.target()) + "U");
        if (i.op == Op::jal) set(31, "sx32(" + hex32(i.pc + 8) + "U)");
        break;
    case Op::jr: case Op::jalr:
        branch("true", "std::uint32_t(" + rs + ")");
        if (i.op == Op::jalr) set(i.rd, "sx32(" + hex32(i.pc + 8) + "U)");
        break;
    case Op::beq: case Op::beql: branch(rs + " == " + rt, hex32(i.target()) + "U"); break;
    case Op::bne: case Op::bnel: branch(rs + " != " + rt, hex32(i.target()) + "U"); break;
    case Op::blez: case Op::blezl: branch("signed64(" + rs + ") <= 0", hex32(i.target()) + "U"); break;
    case Op::bgtz: case Op::bgtzl: branch("signed64(" + rs + ") > 0", hex32(i.target()) + "U"); break;
    case Op::bltz: case Op::bltzl: branch("signed64(" + rs + ") < 0", hex32(i.target()) + "U"); break;
    case Op::bgez: case Op::bgezl: branch("signed64(" + rs + ") >= 0", hex32(i.target()) + "U"); break;
    case Op::lb: case Op::lbu: case Op::lh: case Op::lhu: case Op::lw: case Op::lwu: case Op::ld: {
        unsigned size = (i.op == Op::lb || i.op == Op::lbu) ? 1 : (i.op == Op::lh || i.op == Op::lhu) ? 2 : i.op == Op::ld ? 8 : 4;
        o << "const auto value = bus.load(" << address << ", " << size << ");\n";
        if (i.op == Op::lb) set(i.rt, "std::uint64_t(std::int64_t(std::bit_cast<std::int8_t>(std::uint8_t(value))))");
        else if (i.op == Op::lh) set(i.rt, "std::uint64_t(std::int64_t(std::bit_cast<std::int16_t>(std::uint16_t(value))))");
        else if (i.op == Op::lw) set(i.rt, "sx32(std::uint32_t(value))");
        else set(i.rt, "value");
        break;
    }
    case Op::sb: case Op::sh: case Op::sw: case Op::sd:
        o << "bus.store(" << address << ", " << (i.op == Op::sb ? 1 : i.op == Op::sh ? 2 : i.op == Op::sw ? 4 : 8) << ", " << rt << ");\n"; break;
    case Op::lq: o << "s.set128(" << i.rt << ", bus.load_quad(" << address << "));\n"; break;
    case Op::sq: o << "bus.store_quad(" << address << ", s.gpr[" << i.rt << "]);\n"; break;
    case Op::padduw: o << "s.set128(" << i.rd << ", padduw(s.gpr[" << i.rs << "], s.gpr[" << i.rt << "]));\n"; break;
    case Op::pand: case Op::por: case Op::pxor: case Op::pnor: {
        const auto a = "s.gpr[" + std::to_string(i.rs) + "]", b = "s.gpr[" + std::to_string(i.rt) + "]";
        const auto symbol = i.op == Op::pand ? " & " : i.op == Op::pxor ? " ^ " : " | ";
        const auto prefix = i.op == Op::pnor ? "~" : "";
        o << "s.set128(" << i.rd << ", {" << prefix << '(' << a << ".lo" << symbol << b << ".lo), "
          << prefix << '(' << a << ".hi" << symbol << b << ".hi)});\n"; break;
    }
    case Op::sync: break; // Single-thread diagnostic bus has no pending operations.
    case Op::syscall: o << "return Result{Stop::syscall, s.pc, word};\n"; break;
    case Op::breakpoint: o << "return Result{Stop::breakpoint, s.pc, word};\n"; break;
    default: o << "return Result{Stop::unsupported, s.pc, word};\n"; break;
    }
}
}
void emit(std::ostream& o, const analysis::Program& p) {
    o << "// Generated by AnyPS2. Bounded native functions, at most 256 instructions each.\n"
         "#include <nt/runtime.hpp>\n#include <optional>\nusing namespace nt::runtime;\n"
         "struct Flow { std::uint32_t next, after = 0, word = 0; bool new_delay = false; };\n"
         "#ifdef _MSC_VER\n#define NT_NOINLINE __declspec(noinline)\n#else\n#define NT_NOINLINE __attribute__((noinline))\n#endif\n";
    std::map<std::uint32_t,std::vector<r5900::Instruction>> pages;
    for (const auto& [pc,i] : p.instructions) pages[pc >> 10].push_back(i);
    for (const auto& [page,instructions] : pages) {
        o << "static NT_NOINLINE std::optional<Result> page_" << page << "(State& s, Bus& bus, Flow& flow) {\n"
             "auto& next = flow.next; auto& after = flow.after; auto& word = flow.word; auto& new_delay = flow.new_delay;\n"
             "const bool was_delay = s.delay_slot;\nswitch(s.pc) {\n";
        for (const auto& i : instructions) {
            o << "case " << hex32(i.pc) << "U: { // " << r5900::name(i.op) << '\n'
              << "word = " << hex32(i.word) << "U;\n"
              << "if (bus.load(s.pc, 4) != word) return Result{Stop::code_changed, s.pc, word};\n";
            if (i.branch()) o << "if (was_delay) return Result{Stop::branch_in_delay, s.pc, word};\n";
            operation(o, i);
            o << "break; }\n";
        }
        o << "default: return Result{Stop::unmapped_code,s.pc,0};\n}\nreturn std::nullopt;\n}\n";
    }
    o << "Result nt_generated_run(State& s, Bus& bus, std::uint64_t budget) {\n"
         "s.gpr[0] = {}; Flow flow{};\ntry { while (budget--) {\n"
         "flow = Flow{s.pc + 4}; const bool was_delay = s.delay_slot;\n"
         "std::optional<Result> stop;\nswitch(s.pc >> 10) {\n";
    for (const auto& [page,instructions] : pages)
        o << "case " << page << ": stop = page_" << page << "(s,bus,flow); break;\n";
    o << "default: return {Stop::unmapped_code,s.pc,0};\n}\nif (stop) return *stop;\n"
         "if (was_delay) flow.next = s.pending_target;\n"
         "if (flow.new_delay) { s.pending_target = flow.after; s.branch_pc = s.pc; }\n"
         "s.delay_slot = flow.new_delay; s.pc = flow.next; ++s.executed;\n"
         "bus.advance_cycles(1); // Diagnostic issue clock: not cycle-accurate EE scheduling.\n"
         "if (!s.delay_slot && (s.cop0_status & 0x10007U) == 0x10001U && (s.cop0_status & bus.interrupt_lines())) return {Stop::interrupt_pending,s.pc,0};\n}\n"
         "} catch (const MemoryFault& error) { return {Stop::memory_fault, s.pc, flow.word, error.what()}; }\n"
         "return {Stop::budget, s.pc, 0};\n}\n";
}
}
