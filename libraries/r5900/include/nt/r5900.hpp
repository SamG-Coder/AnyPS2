#pragma once
#include <cstdint>
#include <string_view>

namespace nt::r5900 {
enum class Op {
    unsupported, pextlb, pextlh, pextlw, pextub, pextuh, pextuw, mov_s, abs_s, neg_s, mul_s, div_s, add_s,
    sub_s, max_s, min_s, cvt_s_w, cvt_w_s, compare_s, bc1f, bc1t, bc1fl, bc1tl, sll, srl, sra, sllv, srlv,
    srav, jr, jalr, movz, movn, syscall, breakpoint, sync, mfhi, mthi, mflo, mtlo, mult, multu, div, divu,
    add, sub, addi, addu, subu, bit_and, bit_or, bit_xor, bit_nor, slt, sltu, daddu, dsubu, dadd, dsub,
    daddi, dsll, dsrl, dsra, dsll32, dsrl32, dsra32, dsllv, dsrlv, dsrav, j, jal, beq, bne, blez, bgtz,
    bltz, bgez, beql, bnel, blezl, bgtzl, bltzl, bgezl, addiu, daddiu, slti, sltiu, andi, ori, xori, lui,
    lb, lbu, lh, lhu, lw, lwu, ld, lq, ldl, ldr, lwl, lwr, sb, sh, sw, sd, sq, sdl, sdr, swl, swr, lwc1,
    swc1, pand, por, pxor, pnor, padduw, pceqb, pceqh, pceqw, ppac5, pext5, ppacb, ppach, ppacw, mfhi1,
    mthi1, mflo1, mtlo1, mfsa, mtsa, mtsab, mtsah, mfc1, mtc1, cfc1, ctc1, cfc2, ctc2, qmfc2, qmtc2, lqc2,
    sqc2, vsub, vadd, vsub_bc, vadd_bc, vmove, vmr32, adda_s, pcpyh, pcpyld, pcpyud, ei, di, mfc0, mtc0,
    eret, plzcw, mult1, multu1, div1, divu1, cache, paddw, psubw, pcgtw, pmaxw, pminw, pabsw, paddsw,
    psubsw, psubuw, paddh, psubh, pcgth, pmaxh, pminh, pabsh, paddsh, psubsh, padduh, psubuh, paddb, psubb,
    pcgtb, paddsb, psubsb, paddub, psubub, padsbh, psllvw, psrlvw, psravw, psllh, psrlh, psrah, psllw,
    psrlw, psraw, madd, maddu, madd1, maddu1, bltzal, bgezal, bltzall, bgezall, pexeh, prevh, pexew, prot3w,
    pexch, pexcw, pinth, pinteh, qfsrv, pmfhi, pmflo, pmthi, pmtlo, pmultw, pmultuw, pdivw, pdivuw, pmadduw,
    vmul, vmul_bc, vmax, vmax_bc, vmini, vmini_bc, vabs, vnop, vitof0, vitof4, vitof12, vitof15, vftoi0,
    vftoi4, vftoi12, vftoi15, suba_s, mula_s, tge, tgeu, tlt, tltu, teq, tne, tgei, tgeiu, tlti, tltiu,
    teqi, tnei, pref, pmfhl, pmthl, sqrt_s,
    pmulth, pmaddh, pmsubh, phmadh, phmsbh, pdivbw, tlbr, tlbwi, tlbwr, tlbp
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
