#pragma once
#include "disc_fixture.hpp"

namespace fixture {
// An initialized descriptor copied with wide loads/stores into a stack frame,
// then forwarded through a helper to two indirect calls. The third word is data.
inline nt::Bytes copied_callback_elf() {
    nt::Bytes bytes(0x500,0);
    const auto put16=[](nt::Bytes& image,unsigned at,unsigned value) {
        image.at(at)=static_cast<std::uint8_t>(value); image.at(at+1)=static_cast<std::uint8_t>(value>>8);
    };
    bytes[0]=0x7f; bytes[1]='E'; bytes[2]='L'; bytes[3]='F'; bytes[4]=bytes[5]=bytes[6]=1;
    put16(bytes,16,2); put16(bytes,18,8); put32(bytes,20,1); put32(bytes,24,0x1000);
    put32(bytes,28,52); put16(bytes,40,52); put16(bytes,42,32); put16(bytes,44,1);
    put32(bytes,52,1); put32(bytes,56,0x100); put32(bytes,60,0x1000);
    put32(bytes,68,0x400); put32(bytes,72,0x800); put32(bytes,76,7); put32(bytes,80,16);
    const auto code=[&](unsigned offset,std::initializer_list<std::uint32_t> words) {
        for (auto word:words) { put32(bytes,offset,word); offset+=4; }
    };
    code(0x100,{0x241d1700,0x0c000410,0,0x2403007b,12});
    code(0x140,{0xffbf0020,0x24041300,0xdc880000,0xdc890008,
        0xffa80010,0x27a40010,0x24020000,0x0c000440,0xffa90018,
        0xdfbf0020,0x03e00008,0});
    code(0x200,{0x27bdfff0,0xffbf0008,0x00802821,0x0c000450,0,
        0xdfbf0008,0x03e00008,0x27bd0010});
    code(0x240,{0x27bdffe0,0xffbf0018,0xffb00010,0x00a08021,
        0x8e190000,0x0320f809,0,0x8e190004,0x0320f809,0,
        0xdfb00010,0xdfbf0018,0x03e00008,0x27bd0020});
    code(0x300,{0x03e00008,0x2442000a});
    code(0x310,{0x03e00008,0x24420020});
    code(0x320,{0x4c000000});
    code(0x400,{0x1200,0x1210,0x1220,0});
    return bytes;
}
// Runtime-initialized 12-byte callback records with a wide count store and
// shift/add indexing. Bounds failure branches back to a shared return path.
inline nt::Bytes scaled_callback_elf() {
    auto bytes=copied_callback_elf();
    std::fill(bytes.begin()+0x100,bytes.end(),0);
    const auto code=[&](unsigned offset,std::initializer_list<std::uint32_t> words) {
        for (auto word:words) { put32(bytes,offset,word); offset+=4; }
    };
    code(0x100,{0x241d1700,0x24041400,0x24081200,0xac880000,
        0x24081210,0xac88000c,0x24081220,0xac880004,
        0x24051450,0xaca40000,0x24080002,0xfca80008,
        0x24040001,0x0c000450,0x24020000,0x2403007b,12});
    code(0x200,{0xdfbf0008,0x03e00008,0x27bd0010});
    code(0x240,{0x27bdfff0,0xffbf0008,0x24081450,0xafa80000,
        0x0c00046c,0,0x8fa80000,0x8d090008,
        0x0089482b,0x1120ffe6,0,0x00041840,0x8d080000,
        0x00641821,0x00031880,0x01031821,0x8c790000,0x0320f809,0,
        0x08000440,0});
    code(0x2b0,{0x03e00008,0});
    code(0x300,{0x03e00008,0x2402000a});
    code(0x310,{0x03e00008,0x2402002a});
    code(0x320,{0x4c000000});
    return bytes;
}
inline nt::Bytes long_frame_callback_elf() {
    auto bytes=copied_callback_elf();
    bytes.resize(0x900,0); std::fill(bytes.begin()+0x100,bytes.end(),0);
    put32(bytes,68,0x800); put32(bytes,72,0x1000);
    const auto code=[&](unsigned offset,std::initializer_list<std::uint32_t> words) {
        for (auto word:words) { put32(bytes,offset,word); offset+=4; }
    };
    code(0x100,{0x241d1f00,0x24040000,0x0c000410,0,0x2403007b,12});
    code(0x140,{0x27bdffe0,0xffbf0018,0xffbe0010,0x03a0f021,
        0x10800007,0,0x24081700,0x10000008,0xafc80000});
    code(0x170,{0x24081710,0xafc80000,0x10000001,0});
    code(0x180,{0x24081720,0xafc80004});
    code(0x188,{0x308300ff,0x2c630002,0x1060011e,0,0x00044880,0x3c080000,
        0x01094021,0x8d191780,0x03200008,0});
    code(0x600,{0x8fd90000,0x0320f809,0,0xdfbe0010,0xdfbf0018,0x03e00008,0x27bd0020});
    code(0x800,{0x03e00008,0x2402000a});
    code(0x810,{0x03e00008,0x2402002a});
    code(0x820,{0x4c000000});
    code(0x880,{0x1200,0x1280});
    return bytes;
}
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
