#include <nt/r5900.hpp>

namespace nt::r5900 {
Instruction decode(std::uint32_t pc, std::uint32_t w) {
    Instruction i{pc, w, Op::unsupported, (w >> 21) & 31, (w >> 16) & 31,
                  (w >> 11) & 31, (w >> 6) & 31,
                  (w & 0x8000) ? std::int32_t(w & 65535) - 65536 : std::int32_t(w & 65535)};
    const auto opcode = w >> 26, fn = w & 63;
    if (opcode == 0) {
        switch (fn) {
#define F(n, operation) case n: i.op = Op::operation; break
        F(0,sll); F(2,srl); F(3,sra); F(4,sllv); F(6,srlv); F(7,srav);
        F(8,jr); F(9,jalr); F(10,movz); F(11,movn); F(12,syscall); F(13,breakpoint);
        F(15,sync); F(16,mfhi); F(17,mthi); F(18,mflo); F(19,mtlo);
        F(20,dsllv); F(22,dsrlv); F(23,dsrav);
        F(24,mult); F(25,multu); F(26,div); F(27,divu);
        F(33,addu); F(35,subu); F(36,bit_and); F(37,bit_or); F(38,bit_xor); F(39,bit_nor);
        F(32,add); F(34,sub);
        F(40,mfsa); F(41,mtsa);
        F(42,slt); F(43,sltu); F(45,daddu); F(47,dsubu);
        F(44,dadd); F(46,dsub);
        F(56,dsll); F(58,dsrl); F(59,dsra); F(60,dsll32); F(62,dsrl32); F(63,dsra32);
        }
    } else if (opcode == 1) {
        switch (i.rt) { F(0,bltz); F(1,bgez); F(2,bltzl); F(3,bgezl); F(24,mtsab); F(25,mtsah); }
    } else if (opcode == 16) {
        if (w == 0x42000038) i.op = Op::ei;
        if (w == 0x42000039) i.op = Op::di;
        if (w == 0x42000018) i.op = Op::eret;
        if (((i.rd == 12 || i.rd == 14 || i.rd == 30) && (w & 0x7ff) == 0) ||
            (i.rd == 25 && (w & 0x7c0) == 0)) {
            if (i.rs == 0) i.op = Op::mfc0;
            if (i.rs == 4) i.op = Op::mtc0;
        }
    } else if (opcode == 17) {
        switch (i.rs) { F(0,mfc1); F(2,cfc1); F(4,mtc1); F(6,ctc1); }
        if (i.rs == 16 && fn == 24) i.op = Op::adda_s;
        if (i.rs == 16 && fn == 2) i.op = Op::mul_s;
        if (i.rs == 16 && fn == 3) i.op = Op::div_s;
        if (i.rs == 16 && fn == 0) i.op = Op::add_s;
        if (i.rs == 16 && fn == 1) i.op = Op::sub_s;
        if (i.rs == 16 && fn == 40) i.op = Op::max_s;
        if (i.rs == 16 && fn == 41) i.op = Op::min_s;
        if (i.rs == 20 && i.rt == 0 && fn == 32) i.op = Op::cvt_s_w;
        if (i.rs == 16 && i.rt == 0 && fn == 36) i.op = Op::cvt_w_s;
        if (i.rs == 16 && i.sa == 0 && (fn == 48 || fn == 50 || fn == 52 || fn == 54)) i.op = Op::compare_s;
        if (i.rs == 8) {
            switch (i.rt) { F(0,bc1f); F(1,bc1t); F(2,bc1fl); F(3,bc1tl); }
        }
        if (i.rs == 16 && i.rt == 0) {
            if (fn == 5) i.op = Op::abs_s;
            if (fn == 6) i.op = Op::mov_s;
            if (fn == 7) i.op = Op::neg_s;
        }
    } else if (opcode == 18) {
        // Bit zero selects interlock. No VU execution is currently accepted,
        // so either form observes an idle unit at the device boundary.
        if ((w & 0x7fe) == 0) {
            if (i.rs == 2) i.op = Op::cfc2;
            if (i.rs == 6) i.op = Op::ctc2;
            if (i.rs == 1) i.op = Op::qmfc2;
            if (i.rs == 5) i.op = Op::qmtc2;
        }
        if (i.rs >= 16 && fn == 44) i.op = Op::vsub;
        if (i.rs >= 16 && fn == 40) i.op = Op::vadd;
        if (i.rs >= 16 && fn < 4) i.op = Op::vadd_bc;
        if (i.rs >= 16 && fn >= 4 && fn < 8) i.op = Op::vsub_bc;
        if (i.rs >= 16 && (w & 0x7ff) == 0x33c) i.op = Op::vmove;
        if (i.rs >= 16 && (w & 0x7ff) == 0x33d) i.op = Op::vmr32;
    } else if (opcode == 28) {
        if (fn == 4 && i.rt == 0 && i.sa == 0) i.op = Op::plzcw;
        if (fn == 8 && i.sa == 30) i.op = Op::pext5;
        if (fn == 8 && i.sa == 31) i.op = Op::ppac5;
        if (fn == 8 && i.sa == 19) i.op = Op::ppacw;
        if (fn == 8 && i.sa == 23) i.op = Op::ppach;
        if (fn == 8 && i.sa == 27) i.op = Op::ppacb;
        if (fn == 8 && i.sa == 18) i.op = Op::pextlw;
        if (fn == 8 && i.sa == 22) i.op = Op::pextlh;
        if (fn == 8 && i.sa == 26) i.op = Op::pextlb;
        if (fn == 40 && i.sa == 18) i.op = Op::pextuw;
        if (fn == 40 && i.sa == 22) i.op = Op::pextuh;
        if (fn == 40 && i.sa == 26) i.op = Op::pextub;
        switch (fn) { F(16,mfhi1); F(17,mthi1); F(18,mflo1); F(19,mtlo1); }
        if (fn == 9 && i.sa == 18) i.op = Op::pand;
        if (fn == 9 && i.sa == 19) i.op = Op::pxor;
        if (fn == 41 && i.sa == 18) i.op = Op::por;
        if (fn == 41 && i.sa == 19) i.op = Op::pnor;
        if (fn == 40 && i.sa == 16) i.op = Op::padduw;
        if (fn == 40 && i.sa == 2) i.op = Op::pceqw;
        if (fn == 40 && i.sa == 6) i.op = Op::pceqh;
        if (fn == 40 && i.sa == 10) i.op = Op::pceqb;
        if (fn == 41 && i.sa == 27) i.op = Op::pcpyh;
        if (fn == 9 && i.sa == 14) i.op = Op::pcpyld;
        if (fn == 41 && i.sa == 14) i.op = Op::pcpyud;
    } else if (opcode == 47) {
        if (i.rt == 0x18 || i.rt == 0x1a || i.rt == 0x1c) i.op = Op::cache;
    } else {
        switch (opcode) {
        F(2,j); F(3,jal); F(4,beq); F(5,bne); F(6,blez); F(7,bgtz);
        F(9,addiu); F(10,slti); F(11,sltiu); F(12,andi); F(13,ori); F(14,xori); F(15,lui);
        F(8,addi);
        F(24,daddi);
        F(20,beql); F(21,bnel); F(22,blezl); F(23,bgtzl); F(25,daddiu);
        F(26,ldl); F(27,ldr);
        F(34,lwl); F(38,lwr); F(42,swl); F(46,swr);
        F(30,lq); F(31,sq); F(32,lb); F(33,lh); F(35,lw); F(36,lbu); F(37,lhu); F(39,lwu);
        F(40,sb); F(41,sh); F(43,sw); F(55,ld); F(63,sd);
        F(49,lwc1); F(57,swc1);
        F(54,lqc2); F(62,sqc2);
        F(44,sdl); F(45,sdr);
#undef F
        }
    }
    return i;
}
bool Instruction::branch() const {
    return (op >= Op::j && op <= Op::bgezl) || (op >= Op::bc1f && op <= Op::bc1tl) || indirect();
}
bool Instruction::likely() const { return (op >= Op::beql && op <= Op::bgezl) || op == Op::bc1fl || op == Op::bc1tl; }
bool Instruction::indirect() const { return op == Op::jr || op == Op::jalr; }
bool Instruction::terminal() const { return op == Op::unsupported || op == Op::breakpoint || op == Op::eret; }
std::uint32_t Instruction::target() const {
    if (op == Op::j || op == Op::jal) return ((pc + 4) & 0xf0000000U) | ((word & 0x03ffffffU) << 2);
    return pc + 4 + static_cast<std::uint32_t>(immediate * 4);
}
std::string_view name(Op op) {
#define N(x) case Op::x: return #x
    switch (op) {
    N(unsupported); N(sll); N(srl); N(sra); N(sllv); N(srlv); N(srav); N(jr); N(jalr); N(movz); N(movn);
    N(syscall); N(breakpoint); N(sync); N(mfhi); N(mthi); N(mflo); N(mtlo);
    N(mult); N(multu); N(div); N(divu);
    N(addu); N(subu); N(bit_and); N(bit_or); N(bit_xor); N(bit_nor); N(slt); N(sltu); N(daddu); N(dsubu);
    N(dsll); N(dsrl); N(dsra); N(dsll32); N(dsrl32); N(dsra32); N(dsllv); N(dsrlv); N(dsrav);
    N(j); N(jal); N(beq); N(bne); N(blez); N(bgtz); N(bltz); N(bgez);
    N(beql); N(bnel); N(blezl); N(bgtzl); N(bltzl); N(bgezl);
    N(addiu); N(daddiu); N(slti); N(sltiu); N(andi); N(ori); N(xori); N(lui);
    N(lb); N(lbu); N(lh); N(lhu); N(lw); N(lwu); N(ld); N(lq); N(ldl); N(ldr); N(sb); N(sh); N(sw); N(sd); N(sq);
    N(pand); N(por); N(pxor); N(pnor); N(padduw);
    N(mfhi1); N(mthi1); N(mflo1); N(mtlo1); N(mfsa); N(mtsa);
    N(mtsab); N(mtsah);
    N(mfc1); N(mtc1); N(cfc1); N(ctc1); N(adda_s);
    N(cfc2); N(ctc2);
    N(lwc1); N(swc1);
    N(qmfc2); N(qmtc2); N(lqc2); N(sqc2); N(vsub);
    N(vmove);
    N(vmr32);
    N(vadd); N(vadd_bc); N(vsub_bc);
    N(mov_s); N(abs_s); N(neg_s); N(mul_s); N(div_s); N(add_s); N(sub_s); N(max_s); N(min_s); N(cvt_s_w); N(cvt_w_s);
    N(compare_s); N(bc1f); N(bc1t); N(bc1fl); N(bc1tl);
    N(pcpyh); N(pcpyld); N(pcpyud); N(ei); N(di); N(mfc0); N(mtc0);
    N(eret); N(plzcw); N(cache);
    N(sdl); N(sdr); N(lwl); N(lwr); N(swl); N(swr); N(add); N(sub); N(addi);
    N(pceqb); N(pceqh); N(pceqw);
    N(ppac5); N(pext5);
    N(ppacb); N(ppach); N(ppacw);
    N(pextlb); N(pextlh); N(pextlw); N(pextub); N(pextuh); N(pextuw);
    N(dadd); N(dsub); N(daddi);
    }
#undef N
    return "unsupported";
}
}
