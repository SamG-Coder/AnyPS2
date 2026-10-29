#pragma once
#include <map>
#include <cstdint>

namespace fixture {
constexpr std::uint32_t imm(unsigned op, unsigned rs, unsigned rt, unsigned value) {
    return (op << 26) | (rs << 21) | (rt << 16) | (value & 65535);
}
constexpr std::uint32_t special(unsigned fn, unsigned rs, unsigned rt, unsigned rd, unsigned sa = 0) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
inline std::map<std::uint32_t, std::uint32_t> words() {
    std::map<std::uint32_t, std::uint32_t> w{
        {0x1000, imm(9,1,2,1)},       // ADDIU wraps/sign extends 32 bits
        {0x1100, imm(15,0,2,0x8000)}, // LUI sign extends
        {0x1200, imm(11,1,2,0xffff)}, // SLTIU sign extends immediate
        {0x1300, (28U<<26)|special(40,1,2,3,16)}, // PADDUW
        {0x1400, imm(4,1,2,3)},      // BEQ -> 1410, slot 1404
        {0x1404, imm(9,3,3,1)}, {0x1408,12}, {0x1410,12},
        {0x1500, imm(20,1,2,3)},     // BEQL false must annul faulting slot
        {0x1504, imm(35,4,3,0)}, {0x1508,12}, {0x1510,12},
        {0x1600, special(9,5,0,5)},  // JALR rd==rs must read target first
        {0x1604, imm(9,3,3,1)}, {0x1700,12},
        {0x1800, imm(35,1,2,0)}, {0x1900, imm(43,1,2,0)},
        {0x1a00, imm(30,1,2,0)}, {0x1b00, imm(31,1,2,0)},
        {0x1c00, imm(9,1,0,1)}, {0x1d00, 0x4c000000},
        {0x1e00, imm(4,0,0,3)}, {0x1e04, imm(4,0,0,3)},
        {0x1f00, imm(4,0,0,3)}, {0x1f04, imm(35,4,3,0)},
        {0x2000, special(3,0,1,2,4)},
        {0x2100, special(41,1,0,0)},
        {0x2200, special(37,1,2,3)},
        {0x2300, imm(32,1,2,0)}, {0x2400, imm(33,1,2,0)},
        {0x2500, imm(55,1,2,0)}, {0x2600, imm(63,1,2,0)},
        {0x2700, (17U<<26)|(4U<<21)|(2U<<16)|(3U<<11)},
        {0x2800, (17U<<26)|(2U<<16)|(3U<<11)},
        {0x2900, 0x46010018},
        {0x2a00, (17U<<26)|(6U<<21)|(2U<<16)|(31U<<11)},
        {0x2b00, (17U<<26)|(2U<<21)|(2U<<16)|(31U<<11)},
        {0x6000, imm(9,4,2,5)}, {0x6004,special(8,31,0,0)}, {0x6008,imm(9,0,7,42)},
        {0x6100,12}, {0x6104,0x4c000000},
        {0x6200,imm(9,0,3,0x91)}, {0x6204,12}, {0x6208,special(8,31,0,0)}, {0x620c,0},
        {0x6300,0x080018c8}, {0x6304,12}, {0x6320,0x4c000000},
        {0x6400,(28U<<26)|special(41,0,2,2,27)},
        {0x6500,(28U<<26)|special(9,1,2,1,14)},
        {0x6600,(28U<<26)|special(41,1,2,2,14)},
        {0x6700,0x42000038}, {0x6800,0x42000039},
        {0x6900,(16U<<26)|(2U<<16)|(12U<<11)},
        {0x6a00,(16U<<26)|(4U<<21)|(2U<<16)|(12U<<11)},
        {0x6b00,0x42000018},
        {0x7000,imm(9,0,16,42)}, {0x7004,imm(9,0,3,0x23)}, {0x7008,12},
        {0x7100,12}, {0x7104,0x4c000000},
        {0x7200,(28U<<26)|special(4,2,0,2)},
        {0x7300,(28U<<26)|special(4,2,0,0)},
        {0x7400,imm(26,1,2,0)}, {0x7500,imm(27,1,2,0)},
        {0x7600,imm(26,1,2,7)}, {0x7604,imm(27,1,2,0)},
        {0x7700,imm(26,2,2,0)},
        {0x7800,0x08001e08}, {0x7804,12}, {0x7820,0x4c000000},
        {0x7900,imm(44,1,2,0)}, {0x7a00,imm(45,1,2,0)},
        {0x7b00,imm(44,1,2,7)}, {0x7b04,imm(45,1,2,0)},
        {0x7c00,special(26,1,2,0)}, {0x7d00,special(27,1,2,0)},
        {0x7e00,special(24,1,2,1)}, {0x7f00,special(25,1,2,1)},
        {0x8400,imm(47,1,0x18,0)}, {0x8404,imm(47,1,0x1a,0)}, {0x8408,imm(47,1,0x1c,0)},
        {0x840c,imm(47,1,0x10,0)},
        {0x8500,imm(9,0,3,0x44)}, {0x8504,imm(9,0,4,1)}, {0x8508,12},
        {0x850c,imm(9,0,16,88)}, {0x8510,imm(9,0,3,0x23)}, {0x8514,12},
        {0x8600,12}, {0x8604,imm(9,0,3,0x42)}, {0x8608,imm(9,0,4,1)}, {0x860c,12}, {0x8610,0x4c000000}
    };
    // Synthetic DMA callbacks and interrupted mainline for the delivery milestone.
    w[0x9000] = imm(43,5,4,0); w[0x9004] = imm(43,5,28,4);
    w[0x9008] = imm(9,0,16,99); w[0x900c] = (17U<<26)|(4U<<21)|(16U<<16)|(3U<<11);
    w[0x9010] = imm(9,0,2,0); w[0x9014] = special(8,31,0,0); w[0x9018] = imm(43,5,16,8);
    w[0x9100] = imm(9,0,2,0xffff); w[0x9104] = special(8,31,0,0); w[0x9108] = imm(43,5,2,12);
    w[0x9200] = 0; w[0x9204] = 0x4c000000;
    w[0x9300] = imm(43,0,4,0x800); w[0x9304] = imm(43,0,28,0x804);
    w[0x9308] = imm(43,0,5,0x808); w[0x930c] = special(8,31,0,0); w[0x9310] = imm(9,0,2,0);
    w[0x9400] = special(20,1,2,2); w[0x9404] = special(22,1,2,3); w[0x9408] = special(23,1,2,4);
    w[0x9500] = imm(34,1,2,0); w[0x9510] = imm(38,1,2,0);
    w[0x9520] = imm(42,1,2,0); w[0x9530] = imm(46,1,2,0);
    w[0x9540] = imm(34,1,2,3); w[0x9544] = imm(38,1,2,0);
    w[0x9600] = special(32,1,2,3); w[0x9610] = special(34,1,2,3); w[0x9620] = imm(8,1,3,1);
    w[0x9700] = (28U<<26)|special(40,1,2,1,10);
    w[0x9710] = (28U<<26)|special(40,1,2,1,6);
    w[0x9720] = (28U<<26)|special(40,1,2,1,2);
    w[0x9800] = (28U<<26)|special(8,0,1,1,31); w[0x9804] = (28U<<26)|special(8,0,1,2,30);
    w[0x9900] = (28U<<26)|special(8,1,2,2,27); w[0x9910] = (28U<<26)|special(8,1,2,2,23); w[0x9920] = (28U<<26)|special(8,1,2,2,19);
    w[0x9a00] = special(44,1,2,3); w[0x9a10] = special(46,1,2,3); w[0x9a20] = imm(24,1,3,1);
    // Compile every supported opcode shape as part of the same generated file.
    for (unsigned operation=0;operation<2;++operation)
        for (unsigned broadcast=0;broadcast<5;++broadcast)
            for (unsigned mask=0;mask<16;++mask) {
                const auto base=0xa000+((operation*5+broadcast)*16+mask)*16;
                const auto fn=broadcast<4 ? broadcast+operation*4 : 40+operation*4;
                w[base]=(18U<<26)|((16U|mask)<<21)|(2U<<16)|(1U<<11)|(2U<<6)|fn;
            }
    for (unsigned mask=0;mask<16;++mask) {
        const auto base=0x9d00+mask*32;
        w[base] = (18U<<26)|(5U<<21)|(1U<<16)|(1U<<11)|1;
        w[base+4] = (18U<<26)|(5U<<21)|(2U<<16)|(2U<<11);
        w[base+8] = (18U<<26)|((16U|mask)<<21)|(2U<<16)|(1U<<11)|(2U<<6)|44;
        w[base+12] = (18U<<26)|(1U<<21)|(3U<<16)|(2U<<11)|1;
    }
    w[0x9f00] = (18U<<26)|(5U<<21)|(1U<<16);
    w[0x9f04] = (18U<<26)|(31U<<21)|44; // VSUB.xyzw vf0,vf0,vf0.
    w[0x9f08] = (18U<<26)|(1U<<21)|(3U<<16);
    w[0x9f20] = imm(54,1,4,0); w[0x9f24] = imm(62,2,4,0);
    w[0x9f30] = (18U<<26)|(26U<<21)|(5U<<16)|(4U<<11)|0x33c;
    w[0x9f34] = (18U<<26)|(31U<<21)|(5U<<16)|(5U<<11)|0x33d;
    for (unsigned reg=0;reg<16;++reg) {
        w[0xab00+reg*16]=(18U<<26)|(6U<<21)|(1U<<16)|(reg<<11)|1;
        w[0xab04+reg*16]=(18U<<26)|(2U<<21)|(2U<<16)|(reg<<11);
    }
    w[0x9c00] = imm(49,1,0,-4); w[0x9c04] = imm(57,1,0,4);
    w[0x9c10] = imm(4,0,0,2); w[0x9c14] = imm(57,1,31,0); w[0x9c1c] = 0;
    w[0xac00]=(17U<<26)|(16U<<21)|(3U<<11)|(4U<<6)|6;
    w[0xac04]=(17U<<26)|(16U<<21)|(4U<<11)|(4U<<6)|5;
    w[0xac08]=(17U<<26)|(16U<<21)|(4U<<11)|(5U<<6)|7;
    w[0xae00]=imm(4,0,0,2);
    w[0xae04]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(1U<<6)|2;
    w[0xae0c]=0;
    w[0xaf00]=imm(4,0,0,3); w[0xaf04]=12;
    w[0xaf10]=imm(9,16,16,1); w[0xaf14]=0x4c000000;
    w[0xaf40]=imm(9,0,3,0xffcc); w[0xaf44]=imm(9,0,4,1); w[0xaf48]=12;
    w[0xaf4c]=special(8,31,0,0); w[0xaf50]=0;
    w[0xb500]=imm(4,0,0,2);
    w[0xb504]=(17U<<26)|(20U<<21)|(1U<<11)|(1U<<6)|32;
    w[0xb50c]=0;
    w[0xb520]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(2U<<6)|3;
    w[0xb540]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(2U<<6);
    w[0xb550]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(2U<<6)|1;
    w[0xb560]=(17U<<26)|(16U<<21)|(1U<<11)|(1U<<6)|36;
    w[0xb580]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(1U<<6)|40;
    w[0xb584]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(2U<<6)|41;
    for (unsigned upper=0;upper<2;++upper)
        for (unsigned kind=0;kind<3;++kind)
            w[0xb600+upper*64+kind*16]=(28U<<26)|special(upper ? 40 : 8,1,2,2,26-kind*4);
    for (unsigned selector=0;selector<64;++selector) {
        w[0xb000+selector*16]=(16U<<26)|(4U<<21)|(1U<<16)|(25U<<11)|selector;
        w[0xb004+selector*16]=(16U<<26)|(2U<<16)|(25U<<11)|selector;
    }
    for (unsigned kind=0;kind<4;++kind) {
        const auto base=0xad00+kind*32;
        w[base]=(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(48+kind*2);
        w[base+4]=(17U<<26)|(8U<<21)|(kind<<16)|2;
        w[base+8]=imm(9,4,4,1); w[base+12]=imm(9,5,5,1); w[base+16]=0;
    }
    w[0x9b00] = (18U<<26)|(6U<<21)|(1U<<16)|(28U<<11)|1;
    w[0x9b04] = (18U<<26)|(2U<<21)|(2U<<16)|(28U<<11);
    w[0x9b10] = (18U<<26)|(2U<<21)|(3U<<16)|(29U<<11)|1;
    w[0x9b20] = (18U<<26)|(6U<<21)|(1U<<16)|(31U<<11);
    std::uint32_t pc = 0x20000; // Keep opcode-shape fixtures outside the HLE syscall table.
    for (unsigned op = 0; op < 64; ++op) {
        if (op == 0 || op == 1 || op == 28) continue;
        w[pc] = imm(op,1,2,4); pc += 16;
    }
    for (unsigned fn = 0; fn < 64; ++fn) { w[pc] = special(fn,1,2,3,4); pc += 16; }
    for (unsigned rt = 0; rt < 32; ++rt) { w[pc] = imm(1,1,rt,4); pc += 16; }
    for (unsigned fn : {9U,16U,17U,18U,19U,40U,41U})
        for (unsigned sa : {16U,18U,19U}) { w[pc] = (28U<<26)|special(fn,1,2,3,sa); pc += 16; }
    return w;
}
}
