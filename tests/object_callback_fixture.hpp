#pragma once
#include "disc_fixture.hpp"

namespace fixture {
// Original synthetic object: two virtual methods invoke two separately
// initialized function/context fields. Construction crosses nested helpers;
// one vptr write and one context write occupy branch delay slots.
inline nt::Bytes object_callback_elf() {
    nt::Bytes bytes(0x700,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52;
    bytes[42]=32; bytes[44]=2;
    put32(bytes,24,0x1000); put32(bytes,28,52);
    put32(bytes,52,1); put32(bytes,56,0x100); put32(bytes,60,0x1000);
    put32(bytes,68,0x400); put32(bytes,72,0x400); put32(bytes,76,5); put32(bytes,80,16);
    put32(bytes,84,1); put32(bytes,88,0x500); put32(bytes,92,0x2000);
    put32(bytes,100,0x200); put32(bytes,104,0x400); put32(bytes,108,6); put32(bytes,112,16);
    const auto code=[&](unsigned offset,std::initializer_list<std::uint32_t> words) {
        for (auto word:words) { put32(bytes,offset,word); offset+=4; }
    };
    code(0x100,{0x241d2300,0x24042000,0x0c000440,0x24020000,
        0x24042000,0,0,0x0c000420,0,0x2403007b,12});
    code(0x180,{0x27bdfff0,0xffbf0008,0xffa40000,0x8c830028,
        0x84680000,0x8c790004,0x0320f809,0x00882021,
        0x8fa40000,0x8c830028,0x24630008,0x84680000,
        0x8c790004,0x0320f809,0x00882021,0xdfbf0008,0x03e00008,0x27bd0010});
    code(0x200,{0x27bdfff0,0xffbf0008,0xffb00000,0x00808021,
        0x24082100,0x26040040,0x0c000470,0xae080028,
        0x02002021,0xdfbf0008,0xdfb00000,0x08000460,0x27bd0010});
    code(0x280,{0x24081260,0xac880018,0x24091270,0xac890020,
        0x24081280,0xac88001c,0,0,0x03e00008,0xac800024});
    code(0x2c0,{0x27bdfff0,0xffbf0008,0xffb00000,0x00808021,
        0x0c00047c,0,0xdfb00000,0xdfbf0008,0x03e00008,0x27bd0010});
    code(0x2f0,{0xac800000,0x03e00008,0});
    code(0x300,{0x8c990018,0x03200008,0x8c84001c});
    code(0x320,{0x8c990020,0x03200008,0x8c840024});
    code(0x360,{0x2442000a,0x03e00008,0});
    code(0x370,{0x24420020,0x03e00008,0});
    code(0x380,{0x4c000000}); // Executable-looking opaque data; never called.
    put32(bytes,0x604,0x1200); put32(bytes,0x60c,0x1220);
    return bytes;
}
}
