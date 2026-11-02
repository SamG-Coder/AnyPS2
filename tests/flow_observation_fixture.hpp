#pragma once
#include "disc_fixture.hpp"

namespace fixture {
// Original program with two computed, nested callbacks. Division deliberately
// exceeds the static pointer rules. Each call overwrites its target register in
// the delay slot, so the completed transfer is the relevant observation.
inline nt::Bytes flow_observation_elf() {
    nt::Bytes bytes(0x500,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52;
    bytes[42]=32; bytes[44]=2; bytes[46]=40; bytes[48]=3; bytes[50]=2;
    put32(bytes,24,0x1000); put32(bytes,28,52); put32(bytes,32,0x400);
    put32(bytes,52,1); put32(bytes,56,0x100); put32(bytes,60,0x1000);
    put32(bytes,68,0x240); put32(bytes,72,0x280); put32(bytes,76,7); put32(bytes,80,16);
    put32(bytes,84,1); put32(bytes,88,0x380); put32(bytes,92,0x2000);
    put32(bytes,100,4); put32(bytes,104,32); put32(bytes,108,6); put32(bytes,112,4);
    const std::uint32_t entry[]{
        0x24084400,0x24090004,0x0109001b,0x0000c812, // t9 = 0x4400 / 4
        0x0320f809,0x24190000,0x2403007b,12};       // call; clear t9; stop
    const std::uint32_t first[]{
        0x03e08025,0x24084600,0x0109001b,0x0000c812, // save link; t9 = 0x4600 / 4
        0x0320f809,0x24190000,0x02000008,0x24420002}; // nested call; return result + 2
    const std::uint32_t second[]{0x03e00008,0x24020028}; // return 40
    for (unsigned n=0;n<std::size(entry);++n) put32(bytes,0x100+n*4,entry[n]);
    for (unsigned n=0;n<std::size(first);++n) put32(bytes,0x200+n*4,first[n]);
    for (unsigned n=0;n<std::size(second);++n) put32(bytes,0x280+n*4,second[n]);
    // A VU container remains non-EE content even within an executable segment.
    put32(bytes,0x300,0x4c000000);
    put32(bytes,0x428,1); put32(bytes,0x42c,1); put32(bytes,0x430,6);
    put32(bytes,0x434,0x1200); put32(bytes,0x438,0x300); put32(bytes,0x43c,16); put32(bytes,0x448,16);
    put32(bytes,0x450,9); put32(bytes,0x454,3); put32(bytes,0x460,0x480); put32(bytes,0x464,19);
    const std::string names("\0.vutext\0.shstrtab\0",19);
    std::copy(names.begin(),names.end(),bytes.begin()+0x480);
    return bytes;
}
}
