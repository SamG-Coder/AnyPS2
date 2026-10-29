#pragma once
#include <cstdint>
#include <string_view>

namespace nt::r5900 {
enum class Op {
    unsupported, sll, srl, sra, sllv, srlv, srav, jr, jalr, movz, movn,
    syscall, breakpoint, sync, mfhi, mthi, mflo, mtlo, mult, multu, div, divu,
    add, sub, addi, addu, subu, bit_and, bit_or, bit_xor, bit_nor, slt, sltu, daddu, dsubu, dadd, dsub, daddi,
    dsll, dsrl, dsra, dsll32, dsrl32, dsra32, dsllv, dsrlv, dsrav,
    j, jal, beq, bne, blez, bgtz, bltz, bgez,
    beql, bnel, blezl, bgtzl, bltzl, bgezl,
    addiu, daddiu, slti, sltiu, andi, ori, xori, lui,
    lb, lbu, lh, lhu, lw, lwu, ld, lq, ldl, ldr, lwl, lwr, sb, sh, sw, sd, sq, sdl, sdr, swl, swr,
    pand, por, pxor, pnor, padduw, pceqb, pceqh, pceqw, ppac5, pext5, ppacb, ppach, ppacw, mfhi1, mthi1, mflo1, mtlo1, mfsa, mtsa, mtsab, mtsah,
    mfc1, mtc1, cfc1, ctc1, adda_s, pcpyh, pcpyld, pcpyud, ei, di, mfc0, mtc0, eret, plzcw, cache
};
struct Instruction {
    std::uint32_t pc, word;
    Op op;
    unsigned rs, rt, rd, sa;
    std::int32_t immediate;
    bool branch() const;
    bool likely() const;
    bool indirect() const;
    bool terminal() const;
    std::uint32_t target() const;
};
Instruction decode(std::uint32_t pc, std::uint32_t word);
std::string_view name(Op op);
}
