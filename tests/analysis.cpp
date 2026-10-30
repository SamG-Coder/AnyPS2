#include <nt/analysis.hpp>
#include <iostream>
#include <array>
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
        for (unsigned selector=16;selector<=19;++selector) {
            put(bytes,0,(1U<<26)|(8U<<21)|(selector<<16)|3U);
            put(bytes,4,0x03e0502d); put(bytes,8,12); put(bytes,16,12);
            p=nt::analysis::discover(bytes,elf,64,{},true);
            check(p.instructions.size()==4 && p.instructions.contains(0x1004) &&
                  p.instructions.contains(0x1008) && p.instructions.contains(0x1010),
                  "Conditional-link discovery lost a path or delay slot");
            check(p.instructions.at(0x1000).likely()==(selector>=18),"Conditional-link likely metadata differs");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            // Table-loaded callback passed in a direct wrapper call's delay slot.
            put(image,0,0x24040061); put(image,4,0x24102000);
            put(image,8,0x0c000410); put(image,12,0x8e050000); put(image,16,12);
            put(image,0x40,0x24030074); put(image,0x44,12);
            put(image,0x60,0x03e00008); put(image,0x64,0);
            put(image,0x80,0xa0001060);
            auto program=nt::analysis::discover(image,fixture,128,{},true);
            std::set<std::uint32_t> numbers;
            auto roots=nt::analysis::syscall_callbacks(image,fixture,program,0x74,&numbers);
            check(numbers==std::set<std::uint32_t>{0x61},"Registered syscall continuation lost");
            check(roots==std::set<std::uint32_t>{0x1060},"Table callback or delay-slot argument lost");
            const std::array<std::uint32_t,1> extra{0x1060};
            auto expanded=nt::analysis::discover(image,fixture,128,extra,true);
            check(expanded.instructions.contains(0x1060) && expanded.instructions.contains(0x1064),"Callback root not compiled");
            for (const auto target:{0U,0x1061U,0x2000U,0x3000U}) {
                put(image,0x80,target);
                check(nt::analysis::syscall_callbacks(image,fixture,program,0x74).empty(),"Invalid callback candidate accepted");
            }
            put(image,0x80,0x1060);
            check(nt::analysis::syscall_callbacks(image,fixture,program,0x73).empty(),"Wrong wrapper accepted");
            put(image,4,0x24021060);
            auto pointers=nt::analysis::discover(image,fixture,128,{},true);
            check(nt::analysis::materialized_code_pointers(image,fixture,pointers).contains(0x1060),"Materialized code pointer lost");
            // Argument may also be established before the call, with a nop slot.
            put(image,4,0x24051060); put(image,12,0);
            program=nt::analysis::discover(image,fixture,128,{},true);
            check(nt::analysis::syscall_callbacks(image,fixture,program,0x74)==roots,"Pre-call argument lost");
            put(image,4,0x8c050000); // Unknown/dynamic input is never guessed.
            program=nt::analysis::discover(image,fixture,128,{},true);
            check(nt::analysis::syscall_callbacks(image,fixture,program,0x74).empty(),"Dynamic callback guessed");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x24081060); // entry candidate in t0
            put(image,4,0xafa80024); // descriptor.entry at sp + 0x24
            put(image,8,0x0c000410); put(image,12,0x27a40020);
            put(image,16,12);
            put(image,0x40,0x24030020); put(image,0x44,12);
            put(image,0x60,0x03e00008); put(image,0x64,0);
            const auto roots=[&] {
                return nt::analysis::thread_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060},"Stack thread descriptor entry lost");
            put(image,12,0x03a0202d); put(image,4,0xafa80004);
            check(roots()==std::set<std::uint32_t>{0x1060},"Move-based stack descriptor lost");
            put(image,0,0x27a40020); put(image,4,0x24081060);
            put(image,8,0xafa80024); put(image,12,0x0c000410); put(image,16,0); put(image,20,12);
            check(roots()==std::set<std::uint32_t>{0x1060},"Descriptor store after argument setup lost");
            put(image,8,0x0c000410); put(image,12,0x27a40020); put(image,16,12);
            put(image,4,0xafa80024);
            put(image,0,0x8c080000);
            check(roots().empty(),"Dynamic thread entry guessed");
            put(image,0,0x24081061);
            check(roots().empty(),"Unaligned thread entry accepted");
            put(image,0,0x24082000);
            check(roots().empty(),"Data thread entry accepted");
            put(image,0,0); put(image,4,0); put(image,12,0x24042000);
            put(image,0x84,0x80001060);
            check(roots()==std::set<std::uint32_t>{0x1060},"Static aliased thread entry lost");
            put(image,0x40,0x24030021);
            check(roots().empty(),"Non-create wrapper treated as thread creation");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            // Original callback-table initialization, including a call delay slot.
            put(image,0,0x24081060); put(image,4,0xac88000c);
            put(image,8,0x0c000410); put(image,12,0xac880010); put(image,16,12);
            put(image,0x40,0x03e00008); put(image,0x60,0x03e00008);
            const auto roots=[&] {
                return nt::analysis::stored_code_pointers(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060},"Stored callback not discovered");
            put(image,4,0);
            check(roots()==std::set<std::uint32_t>{0x1060},"Delay-slot stored callback lost");
            for (const auto value:{0U,0x1061U,0x2000U,0x3000U}) {
                put(image,0,0x24080000|value);
                check(roots().empty(),"Invalid stored callback accepted");
            }
            put(image,0,0x8ca80000);
            check(roots().empty(),"Dynamic stored callback guessed");
            put(image,0,0x24081060); put(image,12,0); put(image,16,0xac880010); put(image,20,12);
            check(roots().empty(),"Volatile pointer propagated across call");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x0c000410); put(image,4,0x24051070); put(image,8,12);
            // Copy the argument in a branch delay slot, then store on one path.
            put(image,0x40,0x10800003); put(image,0x44,0x00a0402d);
            put(image,0x48,0xac880000); put(image,0x4c,0);
            put(image,0x50,0x03e00008); put(image,0x54,0);
            put(image,0x70,0x03e00008);
            const auto roots=[&] {
                return nt::analysis::registered_code_pointers(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1070},"Registered argument callback lost");
            put(image,0x44,0x24a80004);
            check(roots().empty(),"Modified argument treated as an exact pointer copy");
            put(image,0x44,0x00a0402d); put(image,0x48,0x8c880000);
            check(roots().empty(),"Loaded value treated as a stored argument");
            put(image,0x48,0xac880000); put(image,4,0x24052000);
            check(roots().empty(),"Non-executable registered argument accepted");
            put(image,4,0x8c850000);
            check(roots().empty(),"Dynamic registered argument guessed");
            put(image,4,0x24051070); put(image,0x40,0x50800003);
            // Likely-false path must not execute the argument-copy delay slot.
            check(roots().empty(),"Annulled argument copy leaked into fallthrough");
            put(image,0x40,0x1000ffff); put(image,0x44,0);
            check(roots().empty(),"Loop summary escaped its reachable graph");
        }
        std::cout << "Control-flow discovery and address-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
