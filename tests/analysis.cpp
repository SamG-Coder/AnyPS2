#include <nt/analysis.hpp>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(nt::Bytes& bytes, std::size_t offset, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(word >> (i * 8));
}
}
int main() {
    try {
        nt::Bytes bytes(32, 0);
        nt::elf::Executable elf{0x1000, 0, {{0,0x1000,32,32,5,4}}};
        put(bytes,0,0x08000404); // J 0x1010
        put(bytes,4,0); // slot
        put(bytes,8,0x4c000000); // unreachable data-like word
        put(bytes,16,0x03e00008); // JR r31, unresolved dynamic destination
        put(bytes,20,0);
        auto p = nt::analysis::discover(bytes,elf);
        check(p.instructions.size() == 4 && !p.instructions.contains(0x1008) && !p.instructions.contains(0x1018), "Jump discovery followed unreachable fallthrough");
        put(bytes,0,0x10000003); // BEQ zero,zero,1010
        p = nt::analysis::discover(bytes,elf);
        check(p.instructions.contains(0x1008), "Conditional fallthrough was not discovered");
        put(bytes,0,0x08000800); // J 2000 outside image
        p = nt::analysis::discover(bytes,elf);
        check(p.unresolved.contains(0x2000) && p.instructions.size() == 2, "Unmapped target lost");
        bool limited = false;
        try { nt::analysis::discover(bytes,elf,1); } catch (const std::runtime_error&) { limited = true; }
        check(limited, "Analysis budget was ignored");
        check(nt::r5900::decode(0x0ffffffc,0x08000000).target() == 0x10000000, "J target used wrong region");
        check(nt::r5900::decode(0x1000,0x1000ffff).target() == 0x1000, "Negative branch displacement failed");
        std::cout << "Control-flow discovery and address-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
