#pragma once
#include "disc_fixture.hpp"

namespace fixture {
// Original synthetic mixed EE/VU-container ELF. The EE function stores a
// payload pointer and reads a word from it; it never calls the payload.
inline nt::Bytes vector_container_elf() {
    nt::Bytes bytes(0x300,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52;
    bytes[42]=32; bytes[44]=2; bytes[46]=40; bytes[48]=3; bytes[50]=2;
    put32(bytes,24,0x1000); put32(bytes,28,52); put32(bytes,32,0x200);
    put32(bytes,52,1); put32(bytes,56,0x100); put32(bytes,60,0x1000);
    put32(bytes,68,0x80); put32(bytes,72,0x100); put32(bytes,76,7); put32(bytes,80,16);
    put32(bytes,84,1); put32(bytes,88,0x180); put32(bytes,92,0x2000);
    put32(bytes,100,4); put32(bytes,104,4); put32(bytes,108,6); put32(bytes,112,4);
    const std::uint32_t program[]{0x34081060,0xac082000,0x8d020000,0x2403007b,12};
    for (unsigned i=0;i<std::size(program);++i) put32(bytes,0x100+i*4,program[i]);
    put32(bytes,0x160,0x12345678); put32(bytes,0x164,0x4c000000);
    put32(bytes,0x228,1); put32(bytes,0x22c,1); put32(bytes,0x230,6);
    put32(bytes,0x234,0x1060); put32(bytes,0x238,0x160); put32(bytes,0x23c,16);
    put32(bytes,0x248,16);
    put32(bytes,0x250,9); put32(bytes,0x254,3); put32(bytes,0x260,0x2a0); put32(bytes,0x264,19);
    const std::string names("\0.vutext\0.shstrtab\0",19);
    std::copy(names.begin(),names.end(),bytes.begin()+0x2a0);
    return bytes;
}
}
