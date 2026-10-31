#include <nt/analysis.hpp>
#include <nt/metrics.hpp>
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
        {
            nt::Bytes image(40,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,40,40,5,4}}};
            put(image,0,(16U<<26)|(8U<<16)|(25U<<11)|1U); // Read counter to r8.
            put(image,4,0x3c090020); // LUI r9,0x20.
            put(image,8,0x250a0001); // ADDIU r10,r8,1.
            put(image,12,0xad2a0010); // SW r10,16(r9).
            put(image,16,0x03e00008); // Return is an unresolved flow boundary.
            put(image,20,0xad280014); // Delay slot is not silently discarded/proven safe.
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites.size()==1 && sites[0].uses.size()==2,"Metric dependency sites lost");
            check(sites[0].uses[0].kind==nt::analysis::MetricUseKind::stored_value &&
                sites[0].uses[0].address==0x200010,"Derived metric store lost its address");
            check(sites[0].uses[1].kind==nt::analysis::MetricUseKind::flow_boundary &&
                sites[0].uses[1].pc==0x1010,"Metric analysis crossed a flow boundary");
            auto bounded=nt::analysis::inspect_metrics(image,fixture,1);
            check(bounded[0].uses.back().kind==nt::analysis::MetricUseKind::budget,"Metric inspection ignored bound");
            put(image,4,0x8d090000); // Counter-derived load address.
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].uses[0].kind==nt::analysis::MetricUseKind::memory_address,"Counter-derived address was hidden");
            put(image,4,0x4c000000);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].uses[0].kind==nt::analysis::MetricUseKind::unknown_operation,"Unknown operation was treated as safe");
            fixture.segments[0].file_size=44;
            bool invalid=false;
            try { nt::analysis::inspect_metrics(image,fixture); } catch (const std::invalid_argument&) { invalid=true; }
            check(invalid,"Malformed metric source segment accepted");
        }
        {
            nt::Bytes image(64,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4}}};
            put(image,0,0x3c090020); // Establish address before the measurement.
            put(image,4,0x25290010);
            put(image,8,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,12,0xad280000); // Counter result stored at 0x200010.
            put(image,16,0x03e00008); put(image,20,0);
            put(image,24,0x3c090020);
            put(image,28,0x912a0011); // Byte read overlaps the counter word.
            put(image,32,0x03e00008); put(image,36,0);
            put(image,40,0x912a0011); // No constant survives the return.
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].uses[0].address==0x200010,"Pre-measurement address was not recovered");
            check(sites[0].memory_uses.size()==2 && sites[0].memory_uses[1].pc==0x101c &&
                !sites[0].memory_uses[1].write && sites[0].memory_uses[1].bytes==1,
                "Overlapping byte consumer lost or stale post-return constant retained");
            // A direct jump to the load invalidates the preceding linear constant.
            put(image,48,0x08000407);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].memory_uses.size()==1,"Address assumption crossed a direct entry target");
            // A malformed later segment must be rejected before following into it.
            fixture.segments.push_back({60,0x2000,16,16,5,4});
            bool invalid=false;
            try { nt::analysis::inspect_metrics(image,fixture); } catch (const std::invalid_argument&) { invalid=true; }
            check(invalid,"Later executable segment was not validated");
        }
        {
            nt::Bytes image(96,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,96,96,5,4}}};
            put(image,0,0x3c090020);
            put(image,4,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,8,0xad280010); // Measurement in a shared structure.
            put(image,12,0x03e00008); put(image,16,0);
            put(image,20,0x3c090020);
            put(image,24,0x0c000418); // JAL, argument established in its slot.
            put(image,28,0x01202021);
            put(image,32,0x0c000418); put(image,36,0); // No constants across the previous call.
            put(image,40,0x3c020020);
            put(image,44,0xafa20000); // Structure pointer escapes through memory.
            put(image,48,0x03e00008); put(image,52,0); // Return the pointer.
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            const auto& uses=sites[0].pointer_uses;
            check(uses.size()==4,"Pointer handoff analysis lost an escape or retained a stale call argument");
            check(uses[0].kind==nt::analysis::MetricPointerKind::call_argument && uses[0].pc==0x1018 &&
                uses[0].reg==4 && uses[0].target==0x1060,"Delay-slot pointer argument was not recorded");
            check(uses[2].kind==nt::analysis::MetricPointerKind::stored_pointer && uses[2].pc==0x102c,
                "Stored structure pointer was not recorded");
            check(uses[3].kind==nt::analysis::MetricPointerKind::return_value && uses[3].reg==2,
                "Returned structure pointer was not recorded");
        }
        {
            // A measured structure escapes through a direct call. Explore both
            // likely-branch paths without applying its slot to the untaken path.
            nt::Bytes image(128,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,128,128,5,4}}};
            put(image,0,0x3c090020);
            put(image,4,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,8,0xad280010);
            put(image,12,0x0c000410); // Callee 0x1040.
            put(image,16,0x01202021); // a0 = structure in delay slot.
            put(image,20,0x03e00008);
            put(image,64,0x50a00003); // BEQL a1,zero,0x1050.
            put(image,68,0x24840004); // Only taken path advances pointer.
            put(image,72,0x8c820010);
            put(image,76,0x08000415); // Join at 0x1054.
            put(image,80,0x8c830010); // Also jump's slot on untaken path.
            put(image,84,0x03e00008);
            put(image,88,0x8c820018); // Return delay slot still consumes pointer.
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            const auto& call=sites[0].pointer_uses[0];
            const auto has=[&](std::uint32_t pc,std::uint32_t address) {
                for (const auto& use:call.memory_uses) if (use.pc==pc && use.address==address) return true;
                return false;
            };
            check(has(0x1048,0x200010) && !has(0x1048,0x200014) &&
                has(0x1050,0x200010) && has(0x1050,0x200014) &&
                has(0x1058,0x200018) && has(0x1058,0x20001c),
                "Callee branch/return delay-slot address candidates differ");
            check(call.boundaries.size()==1 && call.boundaries[0].kind==nt::analysis::MetricUseKind::flow_boundary,
                "Callee return boundary not retained");
            auto bounded=nt::analysis::inspect_metrics(image,fixture,1);
            check(!bounded[0].pointer_uses.empty() && !bounded[0].pointer_uses[0].boundaries.empty() &&
                bounded[0].pointer_uses[0].boundaries[0].kind==nt::analysis::MetricUseKind::budget,
                "Short callee window ignored exploration bound");
            put(image,64,0x0c00041c); // Nested callee stops explicitly.
            put(image,112,0x0000000d);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].pointer_uses[0].memory_uses.empty() &&
                sites[0].pointer_uses[0].boundaries[0].kind==nt::analysis::MetricUseKind::flow_boundary,
                "Unknown nested callee assumed to preserve pointer");
            put(image,64,0x4c000000);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].pointer_uses[0].boundaries[0].kind==nt::analysis::MetricUseKind::unknown_operation,
                "Unsupported callee operation hidden");
            put(image,64,0x24840001); put(image,68,0x08000410); put(image,72,0);
            sites=nt::analysis::inspect_metrics(image,fixture,4);
            bool exhausted=false;
            for (const auto& stop:sites[0].pointer_uses[0].boundaries)
                exhausted|=stop.kind==nt::analysis::MetricUseKind::budget;
            check(exhausted,"Changing-pointer loop ignored callee work bound");
            put(image,64,(6U<<21)|(5U<<16)|(4U<<11)|11U); // MOVN may replace known a0.
            put(image,68,0x8c820010); put(image,72,0x03e00008); put(image,76,0);
            sites=nt::analysis::inspect_metrics(image,fixture);
            const auto& unknown=sites[0].pointer_uses[0];
            bool unresolved=false;
            for (const auto& stop:unknown.boundaries)
                unresolved|=stop.pc==0x1044 && stop.kind==nt::analysis::MetricUseKind::unresolved_memory;
            check(unresolved && unknown.memory_uses.empty(),"Conditional pointer replacement retained stale address");
            put(image,64,(4U<<21)|(5U<<16)|(4U<<11)|10U); // MOVZ with identical choices.
            put(image,68,(49U<<26)|(4U<<21)|(2U<<16)|16U); // LWC1 does not overwrite integer a0.
            put(image,72,(30U<<26)|(4U<<21)|(3U<<16)|31U); // LQ aligns down to 16 bytes.
            put(image,76,(57U<<26)|(4U<<21)|(4U<<16)|32U); // SWC1 is not a GPR pointer escape.
            put(image,80,0x03e00008); put(image,84,0);
            sites=nt::analysis::inspect_metrics(image,fixture);
            const auto& accesses=sites[0].pointer_uses[0].memory_uses;
            check(accesses.size()==3 && accesses[0].address==0x200010 && accesses[0].bytes==4 &&
                accesses[1].address==0x200010 && accesses[1].bytes==16 &&
                accesses[2].address==0x200020 && accesses[2].write,
                "Conditional common pointer or typed memory footprint differs");
            put(image,64,0x0000000d); // Explicit BREAK is a stop, not an unknown instruction.
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].pointer_uses[0].memory_uses.empty() &&
                sites[0].pointer_uses[0].boundaries[0].kind==nt::analysis::MetricUseKind::flow_boundary,
                "Explicit break did not stop callee inspection");

        }
        {
            nt::Bytes image(80,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,80,80,5,4}}};
            put(image,0,0x3c090030); // Parent structure at 0x300000.
            put(image,4,0x252a0020); // Child structure at parent + 32.
            put(image,8,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,12,0xad480010); // Measurement at child + 16.
            put(image,16,0x0c000410); put(image,20,0x01202021); // Pass parent.
            put(image,24,0x03e00008); put(image,28,0);
            put(image,64,0x8c820030); // Callee reads measured field through parent.
            put(image,68,0x03e00008); put(image,72,0);
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            bool found=false;
            for (const auto& pointer:sites[0].pointer_uses) if (pointer.value==0x300000 && pointer.reg==4)
                for (const auto& read:pointer.memory_uses)
                    found|=read.address==0x300030 && read.pc==0x1040 && !read.write;
            check(found && !sites[0].pointer_search_incomplete,"Containing-structure measurement consumer was missed");
            put(image,32,0x3c090030); put(image,36,0x3c0b0040); put(image,40,0xad690020);
            sites=nt::analysis::inspect_metrics(image,fixture);
            bool escaped=false;
            for (const auto& pointer:sites[0].pointer_uses)
                escaped|=pointer.kind==nt::analysis::MetricPointerKind::stored_pointer && pointer.storage==0x400020;
            check(escaped,"Containing structure's stored-pointer destination was lost");

            put(image,4,0x8d2a0000); // Unknown loaded pointer cannot inherit parent ancestry.
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(sites[0].pointer_uses.empty(),"Loaded pointer inherited invented parent structure");
        }
        {
            // Follow an original measured-structure argument through a nested
            // call and its resolved return, including both delay slots.
            nt::Bytes image(160,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,160,160,5,4}}};
            put(image,0,0x3c090020);
            put(image,4,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,8,0xad280010);
            put(image,12,0x0c000410); put(image,16,0x01202021);
            put(image,20,0x0000000d);
            put(image,64,0x0c000420); // JAL 0x1080.
            put(image,68,0x24840004); // Advance argument in call slot.
            put(image,72,0x8c83000c); // Read through callee-modified pointer.
            put(image,76,0x0000000d);
            put(image,128,0x8c82000c);
            put(image,132,0x03e00008);
            put(image,136,0x24840004); // Return slot modifies caller's a0.
            auto sites=nt::analysis::inspect_metrics(image,fixture);
            const auto has=[&](std::uint32_t pc,std::uint32_t address) {
                for (const auto& use:sites[0].pointer_uses[0].memory_uses)
                    if (use.pc==pc && use.address==address) return true;
                return false;
            };
            check(has(0x1080,0x200010) && has(0x1048,0x200014),
                "Nested call lost argument or return-slot effects");
            // A callee overwriting a0 with an unknown load must invalidate it.
            put(image,136,0x8ca40000);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(!has(0x1048,0x200014),"Nested call invented preserved argument");
            bool unknown=false;
            for (const auto& stop:sites[0].pointer_uses[0].boundaries)
                unknown|=stop.pc==0x1048 && stop.kind==nt::analysis::MetricUseKind::unresolved_memory;
            check(unknown,"Unknown returned argument lost its memory boundary");
            // Reading the jump operand before its slot permits a slot to change
            // the link register without changing this return's destination.
            put(image,136,0x241f0000);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(has(0x1048,0x200010),"Return target was read after its delay slot");
            // JALR may use the same register for target and link. Capture the
            // old target before the link write, and publish link before slot.
            put(image,64,0x24191080);
            put(image,68,(25U<<21)|(25U<<11)|9U);
            put(image,72,0x24840004);
            put(image,76,0x8c83000c); put(image,80,0x0000000d);
            put(image,132,(25U<<21)|8U); put(image,136,0x24840004);
            sites=nt::analysis::inspect_metrics(image,fixture);
            check(has(0x1080,0x200010) && has(0x104c,0x200014),
                "Overlapping JALR target/link lost nested consumers");
            // Recursive helper traversal must remain bounded.
            put(image,128,0x0c000420); put(image,132,0);
            sites=nt::analysis::inspect_metrics(image,fixture,4096);
            bool bounded=false;
            for (const auto& stop:sites[0].pointer_uses[0].boundaries)
                bounded|=stop.kind==nt::analysis::MetricUseKind::budget;
            check(bounded,"Recursive metric helper ignored call-depth bound");
        }
        {
            nt::Bytes image(96,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,96,96,5,4}}};
            put(image,0,0x3c090020);
            put(image,4,(16U<<26)|(8U<<16)|(25U<<11)|1U);
            put(image,8,0xad280010);
            put(image,12,0x0c000410); put(image,16,0x01202021);
            put(image,20,0x0000000d);
            put(image,68,0x8c820010); put(image,72,0x0000000d);
            const auto scalar=[](unsigned function) { return (4U<<21)|(4U<<11)|function; };
            const auto packed=[](unsigned function,unsigned sub) {
                return (28U<<26)|(4U<<21)|(4U<<11)|(sub<<6)|function;
            };
            const std::array<std::pair<std::uint32_t,std::uint32_t>,12> operations{{
                {scalar(36),0}, {scalar(37),0x200000}, {scalar(38),0x200000}, {scalar(39),0xffdfffff},
                {packed(9,18),0}, {packed(41,18),0x200000},
                {packed(9,19),0x200000}, {packed(41,19),0xffdfffff},
                {(28U<<26)|(4U<<16)|(4U<<11)|(14U<<6)|9U,0x200000},
                {(28U<<26)|(4U<<16)|(4U<<11)|(27U<<6)|41U,0},
                {(17U<<26)|(16U<<21)|(4U<<11)|(4U<<6)|6U,0x200000}, // MOV.S
                {(18U<<26)|(31U<<21)|(4U<<11)|(4U<<6)|44U,0x200000} // VSUB
            }};
            for (const auto [instruction,value]:operations) {
                put(image,64,instruction);
                const auto sites=nt::analysis::inspect_metrics(image,fixture);
                bool found=false;
                for (const auto& use:sites[0].pointer_uses[0].memory_uses)
                    found|=use.pc==0x1044 && use.address==value+16;
                check(found,"Integer candidate lost across bitwise/copy/independent coprocessor operation");
            }
            for (const auto instruction : {
                    (28U<<26)|(4U<<21)|(4U<<11)|(14U<<6)|41U, // PCPYUD reads unknown upper half.
                    (18U<<26)|(1U<<21)|(4U<<16), // QMFC2 overwrites integer candidate.
                    (18U<<26)|(2U<<21)|(4U<<16)}) { // CFC2 likewise.
                put(image,64,instruction);
                const auto sites=nt::analysis::inspect_metrics(image,fixture);
                const auto& call=sites[0].pointer_uses[0];
                bool unresolved=false;
                for (const auto& stop:call.boundaries)
                    unresolved|=stop.pc==0x1044 && stop.kind==nt::analysis::MetricUseKind::unresolved_memory;
                check(call.memory_uses.empty() && unresolved,"Unknown coprocessor/upper-lane value retained stale pointer");
            }
        }
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
            for (unsigned reg=8;reg<12;++reg) {
                put(image,4,0x24001070U|(reg<<16));
                put(image,0x44,(reg<<21)|(8U<<11)|0x2dU);
                check(roots()==std::set<std::uint32_t>{0x1070},"Extended EE argument callback lost");
            }
            put(image,4,0x240c1070); put(image,0x44,0x0180402d);
            check(roots().empty(),"Non-argument register treated as an ABI input");
            put(image,4,0x24051070);
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
            // Eighth argument saved across a helper call before registration.
            put(image,4,0x240b1070); put(image,0x40,0x0160b82d);
            put(image,0x44,0x0c000418); put(image,0x48,0);
            put(image,0x4c,0xac970000); put(image,0x60,0x03e00008);
            check(roots()==std::set<std::uint32_t>{0x1070},"Preserved argument copy lost across helper call");
            put(image,0x40,0x0160402d); put(image,0x4c,0xac880000);
            check(roots().empty(),"Volatile argument copy survived helper call");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x24082000); put(image,4,0xac880044); // Install table.
            put(image,8,0x8c820044); put(image,12,0x24420018);
            put(image,16,0x84450000); put(image,20,0x8c430004);
            put(image,24,0x0060f809); put(image,28,0); put(image,32,12);
            put(image,0x60,0x03e00008); put(image,0x9c,0x1060);
            const auto roots=[&] {
                return nt::analysis::object_table_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060},"Object table target lost");
            for (auto invalid:{0U,0x1061U,0x2000U}) {
                put(image,0x9c,invalid); check(roots().empty(),"Invalid object table target accepted");
            }
            put(image,0x9c,0x1060); put(image,16,0x8c820000);
            check(roots().empty(),"Clobbered table base propagated");
            put(image,16,0x84450000); put(image,4,0xac880048);
            check(roots().empty(),"Unrelated object field treated as table");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x2c820002); put(image,4,0x1040000a); put(image,8,0);
            put(image,12,0x00041080); put(image,16,0x3c030000); put(image,20,0x00621821);
            put(image,24,0x8c632000); put(image,28,0x00600008); put(image,32,0); put(image,48,12);
            put(image,0x60,0x03e00008); put(image,0x70,0x03e00008);
            put(image,0x80,0x1060); put(image,0x84,0x1070);
            const auto roots=[&] {
                return nt::analysis::bounded_switch_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"Bounded switch entries lost");
            put(image,0x84,0x1071); check(roots().empty(),"Malformed switch accepted partially");
            put(image,0x84,0x1070); put(image,0,0x2ca20002);
            check(roots().empty(),"Unrelated bound supplied switch extent");
            put(image,0,0x2c820002);
            for (unsigned offset=32;offset>=4;offset-=4)
                put(image,offset+12,nt::le32(image,offset));
            put(image,4,0xffb00000); // save s0 without changing index or guard
            put(image,8,0x3c107000); put(image,12,0x36102000);
            check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"Scheduled bound comparison lost switch entries");
            put(image,12,0x34020000);
            check(roots().empty(),"Overwritten guard supplied a switch bound");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x2c850002); put(image,4,0x10a0000a);
            put(image,8,0x3c020000); // Table high in the range-branch delay slot.
            put(image,12,0x00041880); put(image,16,0x24422000);
            put(image,20,0x00621821); put(image,24,0x8c640000);
            put(image,28,0x00800008); put(image,48,12);
            put(image,0x60,0x03e00008); put(image,0x70,0x03e00008);
            put(image,0x80,0x1060); put(image,0x84,0x1070);
            const auto roots=[&] {
                return nt::analysis::bounded_switch_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"Split table-address switch lost");
            put(image,16,0x24822000);
            check(roots().empty(),"Unrelated table low-address source accepted");
            put(image,16,0x24422000); put(image,0x84,0x1071);
            check(roots().empty(),"Split switch accepted a malformed entry");
        }
        std::cout << "Control-flow discovery and address-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
