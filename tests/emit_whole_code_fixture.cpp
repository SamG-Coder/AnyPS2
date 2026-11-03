#include <nt/analysis.hpp>
#include <nt/ee_codegen.hpp>
#include "disc_fixture.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
nt::Bytes fixture_image() {
    nt::Bytes bytes(0x400,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52;
    bytes[42]=32; bytes[44]=1; bytes[46]=40; bytes[48]=5; bytes[50]=4;
    const auto put=[&](std::size_t at,std::uint32_t value) { fixture::put32(bytes,at,value); };
    put(24,0x1000); put(28,52); put(32,0x200);
    put(52,1); put(56,0x100); put(60,0x1000);
    put(68,0x70); put(72,0x80); put(76,7); put(80,16);
    // The host provides a0. Neither callback address occurs in code or data.
    put(0x100,0x00800008); // jr a0
    put(0x110,0x2402002a); put(0x114,0x03e00008);
    put(0x120,0x2402004d); put(0x124,0x03e00008); put(0x128,0x24420001);
    put(0x130,12); // syscall: the native test boundary
    // Deliberate invalid EE words in data, VU and unclassified segment padding.
    put(0x140,0x4c000000); put(0x150,0x4c000000); put(0x160,0x4c000000);
    const auto section=[&](unsigned index,unsigned name,unsigned flags,unsigned address,unsigned size) {
        const auto at=0x200+index*40;
        put(at,name); put(at+4,1); put(at+8,flags); put(at+12,address);
        put(at+16,0x100+address-0x1000); put(at+20,size); put(at+32,4);
    };
    section(1,1,6,0x1000,0x40); section(2,7,3,0x1040,0x10); section(3,13,6,0x1050,0x10);
    put(0x2a0,21); put(0x2a4,3); put(0x2b0,0x300); put(0x2b4,31);
    constexpr char names[]="\0.text\0.data\0.vutext\0.shstrtab\0";
    std::copy(std::begin(names),std::end(names),bytes.begin()+0x300);
    return bytes;
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    try {
        const auto bytes=fixture_image();
        const auto executable=nt::elf::parse(bytes);
        const auto reached=nt::analysis::discover(bytes,executable);
        check(!reached.instructions.contains(0x1010) && !reached.instructions.contains(0x1020),"Fixture accidentally supplied callback roots");
        const auto whole=nt::analysis::whole_code(bytes,executable);
        check(whole.instructions.size()==16 && whole.unresolved.empty(),"Whole-code coverage lost words or promoted non-code storage");
        std::vector<nt::r5900::Instruction> instructions;
        for (const auto& [pc,instruction]:whole.instructions) instructions.push_back(instruction);
        check(nt::ee_codegen::reject_unsupported(instructions).empty(),"Declared code failed lowering");
        auto unknown=bytes; fixture::put32(unknown,0x110,0x4c000000); fixture::put32(unknown,0x120,0x4c000000);
        const auto rejected=nt::analysis::whole_code(unknown,executable);
        instructions.clear();
        for (const auto& [pc,instruction]:rejected.instructions) instructions.push_back(instruction);
        const auto gaps=nt::ee_codegen::reject_unsupported(instructions);
        check(gaps.families.size()==1 && gaps.families.front().count==2,"Unreached unsupported instructions were not rejected together");
        const auto must_reject=[&](nt::elf::Executable malformed,std::size_t limit=100000) {
            bool failed=false;
            try { (void)nt::analysis::whole_code(bytes,malformed,limit); }
            catch (const std::exception&) { failed=true; }
            check(failed,"Ambiguous or malformed whole-code input accepted");
        };
        auto malformed=executable; malformed.code_sections.clear(); must_reject(malformed);
        malformed=executable; malformed.inferred_permissions=true; must_reject(malformed);
        malformed=executable; malformed.code_sections.push_back({0x1050,16}); must_reject(malformed);
        malformed=executable; malformed.code_sections.push_back({0x1010,4}); must_reject(malformed);
        must_reject(executable,15);
        instructions.clear();
        for (const auto& [pc,instruction]:whole.instructions) instructions.push_back(instruction);
        std::ofstream output(argv[1],std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        nt::ee_codegen::emit_llvm(output,instructions);
        nt::write_file(std::string(argv[1])+".elf",bytes);
        nt::write_file(std::string(argv[1])+".unsupported.elf",unknown);
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
