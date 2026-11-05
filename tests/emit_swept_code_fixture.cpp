#include <nt/ee_codegen.hpp>
#include "disc_fixture.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
// One writable, executable section: flow reaches only jr a0 and its delay slot.
// An invalid word splits the remainder; zero fill after it has no transfer.
nt::Bytes mixed_image() {
    nt::Bytes bytes(0x400,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52;
    bytes[42]=32; bytes[44]=1; bytes[46]=40; bytes[48]=3; bytes[50]=2;
    const auto put=[&](std::size_t at,std::uint32_t value) { fixture::put32(bytes,at,value); };
    put(24,0x1000); put(28,52); put(32,0x200);
    put(52,1); put(56,0x100); put(60,0x1000);
    put(68,0x40); put(72,0x40); put(76,7); put(80,16);
    put(0x100,0x00800008); // jr a0
    put(0x110,0x2402002a); put(0x114,0x03e00008);
    put(0x130,12);
    put(0x138,0x4c000000);
    const auto at=0x200+40;
    put(at,1); put(at+4,1); put(at+8,7); put(at+12,0x1000); put(at+16,0x100); put(at+20,0x40); put(at+32,4);
    put(0x250,7); put(0x254,3); put(0x260,0x300); put(0x264,17);
    constexpr char names[]="\0.text\0.shstrtab\0";
    std::copy(std::begin(names),std::end(names),bytes.begin()+0x300);
    return bytes;
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    try {
        const auto word=[](std::uint32_t pc,std::uint32_t value) { return nt::r5900::decode(pc,value); };
        const std::vector<nt::r5900::Instruction> instructions{
            word(0x1000,0x00800008),word(0x1004,0),
            word(0x1010,0x2402002a),word(0x1014,0x03e00008),word(0x1018,0),
            word(0x1030,12)};
        const std::array<nt::ee_codegen::CodeRange,1> swept{{{0x1010,12}}};
        std::ofstream output(argv[1],std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        nt::ee_codegen::emit_llvm(output,instructions,{},{},{},{},{},0,swept);
        nt::write_file(std::string(argv[1])+".elf",mixed_image());
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
