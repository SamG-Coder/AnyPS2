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
        F(48,tge); F(49,tgeu); F(50,tlt); F(51,tltu); F(52,teq); F(54,tne);
        F(40,mfsa); F(41,mtsa);
        F(42,slt); F(43,sltu); F(45,daddu); F(47,dsubu);
        F(44,dadd); F(46,dsub);
        F(56,dsll); F(58,dsrl); F(59,dsra); F(60,dsll32); F(62,dsrl32); F(63,dsra32);
        }
    } else if (opcode == 1) {
        switch (i.rt) {
        F(0,bltz); F(1,bgez); F(2,bltzl); F(3,bgezl);
        F(8,tgei); F(9,tgeiu); F(10,tlti); F(11,tltiu); F(12,teqi); F(14,tnei);
        F(16,bltzal); F(17,bgezal); F(18,bltzall); F(19,bgezall);
        F(24,mtsab); F(25,mtsah);
        }
    } else if (opcode == 16) {
        if (w == 0x42000038) i.op = Op::ei;
        if (w == 0x42000039) i.op = Op::di;
        if (w == 0x42000001) i.op = Op::tlbr;
        if (w == 0x42000002) i.op = Op::tlbwi;
        if (w == 0x42000006) i.op = Op::tlbwr;
        if (w == 0x42000008) i.op = Op::tlbp;
        if (w == 0x42000018) i.op = Op::eret;
        if (((i.rd == 0 || i.rd == 2 || i.rd == 3 || i.rd == 5 || i.rd == 10 || i.rd == 6 || i.rd == 12 || i.rd == 14 || i.rd == 30 || (i.rd == 28 && i.rs == 0)) && (w & 0x7ff) == 0) ||
            (i.rd == 25 && (w & 0x7c0) == 0)) {
            if (i.rs == 0) i.op = Op::mfc0;
            if (i.rs == 4) i.op = Op::mtc0;
        }
    } else if (opcode == 17) {
        switch (i.rs) { F(0,mfc1); F(2,cfc1); F(4,mtc1); F(6,ctc1); }
        if (i.rs == 16 && fn == 25) i.op = Op::suba_s;
        if (i.rs == 16 && fn == 26) i.op = Op::mula_s;
        if (i.rs == 16 && fn == 24) i.op = Op::adda_s;
        if (i.rs == 16 && fn == 2) i.op = Op::mul_s;
        if (i.rs == 16 && fn == 4 && i.rd==0) i.op=Op::sqrt_s;
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
        if (i.rs >= 16) {
            if (fn==42) i.op=Op::vmul;
            if (fn>=24 && fn<28) i.op=Op::vmul_bc;
            if (fn==43) i.op=Op::vmax;
            if (fn>=16 && fn<20) i.op=Op::vmax_bc;
            if (fn==47) i.op=Op::vmini;
            if (fn>=20 && fn<24) i.op=Op::vmini_bc;
            switch (w&0x7ff) {
            F(0x13c,vitof0); F(0x13d,vitof4); F(0x13e,vitof12); F(0x13f,vitof15);
            F(0x17c,vftoi0); F(0x17d,vftoi4); F(0x17e,vftoi12); F(0x17f,vftoi15);
            F(0x1fd,vabs); F(0x2ff,vnop);
            }
        }
        if (i.rs >= 16 && fn == 44) i.op = Op::vsub;
        if (i.rs >= 16 && fn == 40) i.op = Op::vadd;
        if (i.rs >= 16 && fn < 4) i.op = Op::vadd_bc;
        if (i.rs >= 16 && fn >= 4 && fn < 8) i.op = Op::vsub_bc;
        if (i.rs >= 16 && (w & 0x7ff) == 0x33c) i.op = Op::vmove;
        if (i.rs >= 16 && (w & 0x7ff) == 0x33d) i.op = Op::vmr32;
    } else if (opcode == 28) {
        switch (fn) {
        case 8:
            switch (i.sa) {
            F(0,paddw); F(1,psubw); F(2,pcgtw); F(3,pmaxw);
            F(4,paddh); F(5,psubh); F(6,pcgth); F(7,pmaxh);
            F(8,paddb); F(9,psubb); F(10,pcgtb);
            F(16,paddsw); F(17,psubsw); F(18,pextlw); F(19,ppacw);
            F(20,paddsh); F(21,psubsh); F(22,pextlh); F(23,ppach);
            F(24,paddsb); F(25,psubsb); F(26,pextlb); F(27,ppacb);
            F(30,pext5); F(31,ppac5);
            }
            break;
        case 40:
            switch (i.sa) {
            F(1,pabsw); F(2,pceqw); F(3,pminw); F(4,padsbh);
            F(5,pabsh); F(6,pceqh); F(7,pminh); F(10,pceqb);
            F(16,padduw); F(17,psubuw); F(18,pextuw);
            F(20,padduh); F(21,psubuh); F(22,pextuh);
            F(24,paddub); F(25,psubub); F(26,pextub); F(27,qfsrv);
            }
            break;
        case 9:
            switch (i.sa) {
            F(2,psllvw); F(3,psrlvw); F(8,pmfhi); F(9,pmflo);
            F(10,pinth); F(12,pmultw); F(13,pdivw); F(14,pcpyld);
            F(16,pmaddh); F(17,phmadh); F(20,pmsubh); F(21,phmsbh); F(28,pmulth); F(29,pdivbw);
            F(18,pand); F(19,pxor); F(26,pexeh); F(27,prevh);
            F(30,pexew); F(31,prot3w);
            }
            break;
        case 41:
            switch (i.sa) {
            F(0,pmadduw); F(3,psravw); F(8,pmthi); F(9,pmtlo);
            F(10,pinteh); F(12,pmultuw); F(13,pdivuw); F(14,pcpyud);
            F(18,por); F(19,pnor); F(26,pexch); F(27,pcpyh); F(30,pexcw);
            }
            break;
        case 48:
            if (i.sa<=4) i.op=Op::pmfhl;
            break;
        case 49:
            if (!i.sa) i.op=Op::pmthl;
            break;
        default:
            switch (fn) {
            F(16,mfhi1); F(17,mthi1); F(18,mflo1); F(19,mtlo1);
            F(52,psllh); F(54,psrlh); F(55,psrah); F(60,psllw); F(62,psrlw); F(63,psraw);
            }
            if (!i.sa) {
                switch (fn) {
                F(0,madd); F(1,maddu); F(32,madd1); F(33,maddu1);
                F(24,mult1); F(25,multu1); F(26,div1); F(27,divu1);
                }
                if (fn==4 && !i.rt) i.op=Op::plzcw;
            }
        }
    } else if (opcode == 47) {
        if (i.rt == 0x07 || i.rt == 0x0c || i.rt == 0x10 || i.rt == 0x14 || i.rt == 0x18 || i.rt == 0x1a || i.rt == 0x1c) i.op = Op::cache;
    } else {
        switch (opcode) {
        F(51,pref);
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
    return (op >= Op::bltzal && op <= Op::bgezall) || (op >= Op::j && op <= Op::bgezl) || (op >= Op::bc1f && op <= Op::bc1tl) || indirect();
}
bool Instruction::likely() const { return op==Op::bltzall || op==Op::bgezall || (op >= Op::beql && op <= Op::bgezl) || op == Op::bc1fl || op == Op::bc1tl; }
bool Instruction::indirect() const { return op == Op::jr || op == Op::jalr; }
bool Instruction::terminal() const { return op == Op::unsupported || op == Op::breakpoint || op == Op::eret; }
std::uint32_t Instruction::target() const {
    if (op == Op::j || op == Op::jal) return ((pc + 4) & 0xf0000000U) | ((word & 0x03ffffffU) << 2);
    return pc + 4 + static_cast<std::uint32_t>(immediate * 4);
}
std::string_view name(Op op) {
#define N(x) case Op::x: return #x
    switch (op) {
    N(paddw); N(psubw); N(pcgtw); N(pmaxw); N(pminw); N(pabsw); N(paddsw); N(psubsw); N(psubuw); N(paddh);
    N(psubh); N(pcgth); N(pmaxh); N(pminh); N(pabsh); N(paddsh); N(psubsh); N(padduh); N(psubuh); N(paddb);
    N(psubb); N(pcgtb); N(paddsb); N(psubsb); N(paddub); N(psubub); N(padsbh); N(psllvw); N(psrlvw);
    N(psravw); N(psllh); N(psrlh); N(psrah); N(psllw); N(psrlw); N(psraw);
    N(madd); N(maddu); N(madd1); N(maddu1); N(bltzal); N(bgezal); N(bltzall); N(bgezall); N(pexeh); N(prevh);
    N(pexew); N(prot3w); N(pexch); N(pexcw); N(pinth); N(pinteh); N(qfsrv);
    N(pmfhi); N(pmflo); N(pmthi); N(pmtlo); N(pmultw); N(pmultuw); N(pdivw); N(pdivuw); N(pmadduw);
    N(vmul); N(vmul_bc); N(vmax); N(vmax_bc); N(vmini); N(vmini_bc); N(vabs); N(vnop); N(vitof0); N(vitof4);
    N(vitof12); N(vitof15); N(vftoi0); N(vftoi4); N(vftoi12); N(vftoi15); N(suba_s); N(mula_s);
    N(tge); N(tgeu); N(tlt); N(tltu); N(teq); N(tne); N(tgei); N(tgeiu); N(tlti); N(tltiu); N(teqi); N(tnei);
    N(pref);
    N(pmulth); N(pmaddh); N(pmsubh); N(phmadh); N(phmsbh); N(pdivbw);
    N(sqrt_s);
    N(pmfhl); N(pmthl);
    N(unsupported); N(sll); N(srl); N(sra); N(sllv); N(srlv); N(srav); N(jr); N(jalr); N(movz); N(movn);
    N(syscall); N(breakpoint); N(sync); N(mfhi); N(mthi); N(mflo); N(mtlo);
    N(mult); N(multu); N(div); N(divu); N(mult1); N(multu1); N(div1); N(divu1);
    N(addu); N(subu); N(bit_and); N(bit_or); N(bit_xor); N(bit_nor); N(slt); N(sltu); N(daddu); N(dsubu);
    N(dsll); N(dsrl); N(dsra); N(dsll32); N(dsrl32); N(dsra32); N(dsllv); N(dsrlv); N(dsrav);
    N(j); N(jal); N(beq); N(bne); N(blez); N(bgtz); N(bltz); N(bgez);
    N(beql); N(bnel); N(blezl); N(bgtzl); N(bltzl); N(bgezl);
    N(addiu); N(daddiu); N(slti); N(sltiu); N(andi); N(ori); N(xori); N(lui);
    N(lb); N(lbu); N(lh); N(lhu); N(lw); N(lwu); N(ld); N(lq); N(ldl); N(ldr); N(sb); N(sh); N(sw); N(sd);
    N(sq);
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
    N(mov_s); N(abs_s); N(neg_s); N(mul_s); N(div_s); N(add_s); N(sub_s); N(max_s); N(min_s); N(cvt_s_w);
    N(cvt_w_s);
    N(compare_s); N(bc1f); N(bc1t); N(bc1fl); N(bc1tl);
    N(tlbr); N(tlbwi); N(tlbwr); N(tlbp);
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
