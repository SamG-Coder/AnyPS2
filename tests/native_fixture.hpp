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
