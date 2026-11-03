#include <nt/analysis.hpp>
#include <nt/metrics.hpp>
#include "object_callback_fixture.hpp"
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
            auto image=fixture::object_callback_elf();
            const auto executable=nt::elf::parse(image);
            const auto roots=[&] {
                const auto first=nt::analysis::discover(image,executable,2000,{},true);
                const auto methods=nt::analysis::object_table_entries(image,executable,first);
                const std::vector entries(methods.begin(),methods.end());
                return nt::analysis::stored_code_pointers(image,executable,
                    nt::analysis::discover(image,executable,2000,entries,true));
            };
            const std::set<std::uint32_t> both{0x1260,0x1270},second{0x1270};
            check(roots()==both,"Virtual methods did not establish initialized object callbacks");
            for (unsigned offset=0x1c4;offset>=0x190;offset-=4) put(image,offset+4,nt::le32(image,offset));
            put(image,0x190,0x24050007);
            check(roots()==both,"Scheduled scalar work hid a virtual callback consumer");
            image=fixture::object_callback_elf();
            for (const auto overwrite:{0xa0800019U,0xa480001aU,0xac800018U,0xfc800018U,0xb080001bU}) {
                put(image,0x298,overwrite);
                check(roots()==second,"An overwritten object callback was resurrected");
            }
            image=fixture::object_callback_elf();
            put(image,0x298,0x00802821); put(image,0x29c,0xaca00018);
            check(roots()==second,"A copied object pointer hid a callback overwrite");
            image=fixture::object_callback_elf();
            put(image,0x2a0,0x0800047c); put(image,0x2f0,0xac800018);
            check(roots()==second,"A tail helper clearing a callback was ignored");
            image=fixture::object_callback_elf();
            const std::uint32_t linked_helper[]{0x27bdfff0,0xffbf0008,
                0x24081260,0xac880018,0x24091270,0xac890020,
                0x0c00047c,0,0xdfbf0008,0x03e00008,0x27bd0010};
            for (unsigned n=0;n<std::size(linked_helper);++n) put(image,0x280+n*4,linked_helper[n]);
            check(roots()==both,"A direct helper return lost intact callback definitions");
            put(image,0x2f0,0xac800018);
            check(roots()==second,"A linking helper's callback overwrite was ignored");
            image=fixture::object_callback_elf();
            put(image,0x298,0xac800028);
            check(roots().empty(),"An overwritten vptr retained the old method layout");
            image=fixture::object_callback_elf();
            put(image,0x284,0xaca80018); put(image,0x28c,0xaca90020);
            check(roots().empty(),"A different object inherited constructor callback fields");
            image=fixture::object_callback_elf();
            put(image,0x19c,0x00a02021); put(image,0x1b8,0x00a02021);
            check(roots().empty(),"Virtual dispatch with a different this pointer established fields");
            image=fixture::object_callback_elf();
            put(image,0x604,0x1240); put(image,0x60c,0x1240); put(image,0x340,0x03e00008);
            check(roots().empty(),"An unrelated virtual table established callback fields");
            image=fixture::object_callback_elf();
            put(image,0x300,0xac800018); put(image,0x304,0x8c990018);
            put(image,0x308,0x03200008); put(image,0x30c,0x8c84001c);
            check(roots()==second,"A method clearing its input field established an incoming callback");
            image=fixture::object_callback_elf();
            put(image,0x600,8); put(image,0x608,8);
            put(image,0x300,0x8c990010); put(image,0x320,0x8c990018);
            check(roots()==both,"Positive this adjustment did not normalize callback fields");
            put(image,0x600,0xfff8); put(image,0x608,0xfff8);
            put(image,0x300,0x8c990020); put(image,0x320,0x8c990028);
            check(roots()==both,"Signed negative this adjustment did not normalize callback fields");
            image=fixture::object_callback_elf();
            std::fill(image.begin()+0x280,image.begin()+0x2b0,0);
            const std::uint32_t split[]{0x10a00005,0,0x24081260,0xac880018,
                0xac800028,0,0x03e00008,0};
            for (unsigned n=0;n<std::size(split);++n) put(image,0x280+n*4,split[n]);
            check(roots().empty(),"Disjoint live-vptr and callback paths were combined");
        }
        {
            // A factory returns an object containing a copied callback record.
            // The caller saves/reloads that object before replacing its function.
            nt::Bytes image(512,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,512,512,5,4}}};
            const std::uint32_t caller[]{0x0c000440,0,0xae020000,0x240811e0,0,
                0x8e030000,0xac680018,0x0c000460,0x00602021,12};
            const std::uint32_t wrapper[]{0x27bdfff0,0xffbf0008,0x0c000450,0,
                0xdfbf0008,0x03e00008,0x27bd0010};
            const std::uint32_t factory[]{0x00861021,0xdca80000,0xfc480018,0,
                0x03e00008,0};
            const std::uint32_t consumer[]{0x8c990018,0x03200008,0x8c84001c};
            const auto install=[&](unsigned offset,const auto& code) {
                for (unsigned n=0;n<std::size(code);++n) put(image,offset+n*4,code[n]);
            };
            install(0,caller); install(0x100,wrapper); install(0x140,factory); install(0x180,consumer);
            put(image,0x1e0,0x03e00008);
            const auto roots=[&] { return nt::analysis::stored_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,512,{},true)); };
            check(roots().contains(0x11e0),"Callback on a saved factory result was not recovered");
            for (const auto overwrite:{0xa2000001U,0xae000000U,0xaa000003U}) {
                put(image,16,overwrite);
                check(roots().empty(),"Overwritten factory-result spill retained object identity");
            }
            put(image,16,0); put(image,24,0xac68001c);
            check(roots().empty(),"The opaque field on a factory result became callable");
            put(image,24,0xac680018); put(image,0x14c,0x24020000);
            check(roots().empty(),"A different returned object inherited the copied record");
            put(image,0x14c,0); put(image,0x148,0xac480018);
            check(roots().empty(),"An incomplete record copy established a factory type");
            put(image,0x148,0xfc480014);
            check(roots().empty(),"An unrelated factory field became a callback record");
            put(image,0x148,0xfc480018); put(image,0x110,0x24020000);
            check(roots().empty(),"A wrapper clobber retained the callee return identity");
        }
        {
            // A pointer store alone does not establish a callback. A partial
            // overwrite must sever the local store-to-call relation.
            nt::Bytes image(128,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,128,128,5,4}}};
            put(image,0,0x24081070); put(image,4,0xac88000c); put(image,12,12);
            put(image,112,0x03e00008);
            std::vector<nt::analysis::PointerReference> references;
            const auto roots=[&] {
                references.clear();
                return nt::analysis::stored_code_pointers(image,fixture,
                    nt::analysis::discover(image,fixture,128,{},true),&references);
            };
            check(roots().empty() && references.size()==1 && !references[0].callable,
                "An unconsumed object payload became code or disappeared from the report");
            put(image,12,0x8c99000c); put(image,16,0x0320f809); put(image,24,12);
            check(roots()==std::set<std::uint32_t>{0x1070} && references[0].callable,
                "Store/load/indirect-call evidence was not recovered");
            for (const auto overwrite:{0xa080000dU,0xa480000eU,0xac80000cU,0xfc800008U,0x7c800000U,0xe480000cU}) {
                put(image,8,overwrite);
                check(roots().empty(),"An overwritten object slot retained a stale callback");
            }
            put(image,8,0); put(image,12,0x9099000c);
            check(roots().empty(),"A byte payload read was classified as a function pointer");
        }
        {
            // The ninth argument is loaded from the incoming stack, including
            // a callee frame adjustment and a caller delay-slot store.
            nt::Bytes image(128,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,128,128,5,4}}};
            put(image,0,0x24081070); put(image,4,0xafa80000);
            put(image,12,0x0c000410); put(image,20,12);
            put(image,64,0x27bdfff0); put(image,68,0x8fb90010);
            put(image,72,0x03200008); put(image,76,0x27bd0010); put(image,112,0x03e00008);
            const auto roots=[&] { return nt::analysis::registered_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,128,{},true)); };
            check(roots()==std::set<std::uint32_t>{0x1070},"Stack argument callback was not recovered");
            for (const auto overwrite:{0xa3a00001U,0xa7a00002U,0xafa00000U,0xffa00000U,0x7fa00000U,0xe7a00000U}) {
                put(image,8,overwrite);
                check(roots().empty(),"Overwritten outgoing stack argument retained a callback");
            }
            put(image,8,0); put(image,4,0); put(image,16,0xafa80000);
            check(roots()==std::set<std::uint32_t>{0x1070},"Delay-slot stack argument was lost");
            put(image,64,0xafa00000); put(image,68,0x8fb90000); put(image,76,0);
            check(roots().empty(),"Overwritten incoming argument was resurrected");
            put(image,64,0); put(image,68,0x8fa80000); put(image,72,0x91020000); put(image,76,0x03e00008);
            std::vector<nt::analysis::PointerReference> references;
            const auto data=nt::analysis::registered_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,128,{},true),&references);
            check(data.empty() && references.size()==1 && references[0].data_read && !references[0].callable,
                "A stack payload read was not distinguished from a callback");
        }
        {
            nt::Bytes image(48,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,16,16,5,4},{16,0x2000,32,32,6,4}}};
            put(image,0,0x0c000800); put(image,8,12);
            put(image,16,0x03e00008); put(image,20,0x2402002a);
            put(image,32,0x0c000c00); // Unreferenced data must remain unexamined.
            const auto declared=nt::analysis::discover(image,fixture,32,{},true);
            check(declared.unresolved.contains(0x2000),"Ordinary ELF data was treated as code");
            fixture.inferred_permissions=true;
            check(!nt::elf::executable_address(fixture,0x2000) && nt::elf::executable_address(fixture,0x2000,4,true),
                "Inferred permissions promoted arbitrary data-pointer candidates");
            const auto imported=nt::analysis::discover(image,fixture,32,{},true);
            check(imported.unresolved.empty() && imported.instructions.contains(0x2000) &&
                  !imported.instructions.contains(0x2010),"Imported code permissions were not derived from flow");
        }
        {
            nt::Bytes image(160,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,160,160,5,4}}};
            put(image,0,0x0c000408); put(image,4,0x24051090); put(image,8,12);
            put(image,32,0x0c000410); put(image,36,0x00a0302d); put(image,40,0x03e00008);
            put(image,64,0xac860000); put(image,68,0x8c990000); put(image,72,0x03200008);
            put(image,144,0x03e00008);
            const auto entries=[&] { return nt::analysis::registered_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,160,{},true)); };
            check(entries().contains(0x1090),"Callback forwarded through a registration helper was not recovered");
            put(image,36,0x8ca60000);
            check(entries().empty(),"Loaded data was treated as the forwarded callback argument");
        }
        {
            // A wrapper selects two callbacks and tail-calls their consumer.
            // Restore the link before the jump and set an argument in its slot.
            nt::Bytes image(160,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,160,160,5,4}}};
            put(image,0,0x24051080); put(image,4,0xdfbf0000);
            put(image,8,0x08000410); put(image,12,0x24061090);
            put(image,64,0xe7b40020); put(image,68,0xafa50000);
            put(image,72,0xafa60004); put(image,76,0x8fb90000); put(image,80,0x0320f809);
            put(image,88,0x8fb90004); put(image,92,0x03200008);
            put(image,128,0x03e00008); put(image,144,0x03e00008);
            const auto entries=[&] { return nt::analysis::registered_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,160,{},true)); };
            check(entries()==std::set<std::uint32_t>{0x1080,0x1090},"Tail-call callback arguments were not recovered");
            for (const auto selector:{16U,17U,18U,19U}) {
                put(image,8,(1U<<26)|(8U<<21)|(selector<<16)|13U);
                check(entries()==std::set<std::uint32_t>{0x1080,0x1090},"Conditional-link callback arguments were lost");
            }
            // Initial-data and constant-register calls share discovery, including
            // JALR with a nonstandard link register or no link register at all.
            put(image,4,0x24191040);
            for (const auto jump:{0x0320f809U,0x0320c809U,0x03200009U,0x03200008U}) {
                put(image,8,jump);
                const auto first=nt::analysis::discover(image,fixture,160,{},true);
                const auto roots=nt::analysis::initial_indirect_entries(image,fixture,first);
                const std::vector<std::uint32_t> seeds(roots.begin(),roots.end());
                const auto expanded=nt::analysis::discover(image,fixture,160,seeds,true);
                check(nt::analysis::registered_code_pointers(image,fixture,expanded)==std::set<std::uint32_t>{0x1080,0x1090},
                    "Register call or tail-call callback arguments were lost");
                const auto sites=nt::analysis::call_sites(image,fixture,expanded);
                check(!sites.empty() && sites.front().pc==0x1008 && sites.front().target==0x1040 &&
                    sites.front().initial_data_candidate,"Indirect call inventory lost target provenance");
            }
            put(image,8,0x08000410);
            put(image,4,0xdfa50000);
            check(entries()==std::set<std::uint32_t>{0x1090},"Restored argument retained a stale tail-call target");
            put(image,12,0x24061091);
            check(entries().empty(),"Unaligned tail-call callback was accepted");
            put(image,12,0x24061090); put(image,64,0x08000410); put(image,68,0);
            check(entries().empty(),"Recursive tail jump invented callback consumption");
        }
        {
            nt::Bytes image(128,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,128,128,5,4}}};
            put(image,0,0x0c000410); put(image,8,0x0040f809); put(image,16,12);
            put(image,64,0x24021070); put(image,68,0x03e00008); put(image,112,0x03e00008);
            const auto entries=[&] { return nt::analysis::initial_indirect_entries(image,fixture,
                nt::analysis::discover(image,fixture,128,{},true)); };
            check(entries()==std::set<std::uint32_t>{0x1070},"Returned callback was not discovered");
            put(image,64,0); put(image,72,0x24021070);
            check(entries()==std::set<std::uint32_t>{0x1070},"Return delay-slot callback was not discovered");
            put(image,72,0x8ca20000);
            check(entries().empty(),"Unknown returned pointer was guessed");
            put(image,72,0x24021070); put(image,8,12);
            check(entries().empty(),"Unconsumed return value was treated as executable code");
        }
        {
            nt::Bytes image(64,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4}}};
            put(image,0,0x24031030); put(image,4,0xdfb00000); put(image,8,0xac830000);
            put(image,12,0x8c990000); put(image,16,0x0320f809); put(image,24,12);
            put(image,48,0x03e00008);
            const auto entries=[&] { return nt::analysis::stored_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,64,{},true)); };
            check(entries().contains(0x1030),"Unrelated register restore hid a stored callback");
            fixture.non_executable_sections.push_back({0x1030,4});
            check(entries().empty(),"Data-section address inside an executable LOAD became a callback");
            const std::array<std::uint32_t,1> data_entry{0x1030};
            bool rejected=false;
            try { nt::analysis::discover(image,fixture,64,data_entry,true); }
            catch (const std::runtime_error&) { rejected=true; }
            check(rejected,"An explicit data-section address was accepted as an executable root");
            fixture.non_executable_sections.clear();
            put(image,4,0xdfa30000);
            check(entries().empty(),"Restored callback register retained its prior constant");
            put(image,4,0x7bb00000);
            check(entries().contains(0x1030),"Unrelated quadword restore hid a stored callback");
            put(image,4,0x7ba30000);
            check(entries().empty(),"Quadword restore retained its prior callback constant");
        }
        {
            nt::Bytes image(128,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,128,128,5,4}}};
            put(image,0,0x0c000408); put(image,4,0x24051070); put(image,8,12);
            const std::uint32_t visitor[]{0x00a0882d,0x0c000418,0,0x0220f809,0,0x03e00008,0};
            for (unsigned n=0;n<std::size(visitor);++n) put(image,32+n*4,visitor[n]);
            put(image,96,0x03e00008); put(image,112,0x03e00008);
            const auto entries=[&] { return nt::analysis::registered_code_pointers(image,fixture,
                nt::analysis::discover(image,fixture,128,{},true)); };
            check(entries()==std::set<std::uint32_t>{0x1070},"Invoked argument callback was not recovered across a helper call");
            for (const auto word:{0x46000000U,0x44840000U,0x44080000U}) {
                put(image,40,word);
                check(entries()==std::set<std::uint32_t>{0x1070},"Unrelated scalar floating-point work hid a callback");
            }
            put(image,40,0x44110000);
            check(entries().empty(),"Floating-point read retained a stale callback register");
            put(image,40,0);
            put(image,44,0x8c910000); put(image,48,0x0220f809);
            check(entries().empty(),"Invoked argument survived an unknown pointer overwrite");
        }
        {
            nt::Bytes image(176,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,160,160,5,4},{160,0x2000,16,16,6,4}}};
            put(image,0,0x0c000404); put(image,8,12);
            const std::uint32_t consumer[]{0x0080802d,0x8e190000,0x0320f809,0,0x8e190008,0x0320f809,0,0x03e00008,0};
            for (unsigned n=0;n<std::size(consumer);++n) put(image,16+n*4,consumer[n]);
            put(image,128,0x03e00008); put(image,144,0x03e00008);
            put(image,160,0x1080); put(image,168,0x1090);
            const auto program=nt::analysis::discover(image,fixture,128,{},true);
            const auto entries=[&] { return nt::analysis::registered_code_pointers(image,fixture,program); };
            check(entries()==std::set<std::uint32_t>{0x1080,0x1090},"Consumer-shaped callback record was not recovered");
            put(image,168,0x2000); check(entries().empty(),"Malformed callback record contributed partial roots");
            put(image,168,0x1090); fixture.segments[1].file_size=10;
            check(entries().empty(),"Truncated callback record contributed roots");
        }
        {
            nt::Bytes image(96,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4},{64,0x2000,16,16,6,4}}};
            const std::array<unsigned,7> words{0x24032000,0x84640000,0x8c650004,0x00a0f809,0,12,0};
            for (unsigned n=0;n<words.size();++n) put(image,n*4,words[n]);
            put(image,32,0x03e00008); put(image,64,7); put(image,68,0x1020);
            const auto program=nt::analysis::discover(image,fixture,64,{},true);
            check(nt::analysis::initial_indirect_entries(image,fixture,program)==std::set<std::uint32_t>{0x1020},
                "Argument-register callback was lost across unrelated halfword load");
            put(image,68,0x2000);
            check(nt::analysis::initial_indirect_entries(image,fixture,program).empty(),"Data target accepted as code");
            put(image,68,0x1021);
            check(nt::analysis::initial_indirect_entries(image,fixture,program).empty(),"Unaligned indirect target accepted");
            put(image,68,0x1020); fixture.segments[1].file_size=6;
            check(nt::analysis::initial_indirect_entries(image,fixture,program).empty(),"Truncated indirect pointer accepted");
            fixture.segments[1].file_size=16; image.resize(70);
            check(nt::analysis::initial_indirect_entries(image,fixture,program).empty(),"Missing initial-data bytes accepted");
        }
        {
            nt::Bytes image(96,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4},{64,0x2000,16,16,6,4}}};
            put(image,0,0x24102000); put(image,4,0x8e020000); put(image,8,12);
            put(image,32,0x03e00008); put(image,48,0x03e00008);
            put(image,64,0xffffffff); put(image,68,0x1020); put(image,72,0x1030);
            const auto program=nt::analysis::discover(image,fixture,64,{},true);
            const auto entries=[&] { return nt::analysis::referenced_lifecycle_entries(image,fixture,program); };
            check(entries()==std::set<std::uint32_t>{0x1020,0x1030},"Referenced sentinel lifecycle list lost callbacks");
            put(image,64,2); check(entries().size()==2,"Counted lifecycle list lost callbacks");
            put(image,64,1); check(entries().empty(),"Inconsistent lifecycle count accepted");
            put(image,64,0xffffffff); put(image,72,0x2000);
            check(entries().empty(),"Data pointer accepted as a lifecycle callback");
            put(image,72,0x1031); check(entries().empty(),"Misaligned lifecycle target accepted");
            put(image,72,0x1030); fixture.segments[1].file_size=12;
            check(entries().empty(),"Unterminated lifecycle list accepted");
            fixture.segments[1].file_size=16;
            auto unreferenced=program; unreferenced.instructions.erase(0x1000);
            check(nt::analysis::referenced_lifecycle_entries(image,fixture,unreferenced).empty(),"Unreferenced data scanned as callbacks");
        }
        {
            // Original synthetic program: indirect initial-data pointer and
            // ABI layout-table arguments, with a saved register across a branch.
            nt::Bytes image(160,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4},{64,0x2000,96,96,6,4}}};
            const std::array<unsigned,12> words{0x24102000,0x8e100000,0x10000001,0,
                0x8e060004,0x8e070008,0x8e08000c,0x0c00040c,0,12,0,0};
            for (unsigned n=0;n<words.size();++n) put(image,n*4,words[n]);
            put(image,48,0x24030056); put(image,52,12); put(image,56,0x03e00008);
            put(image,64,0x2020);
            put(image,96,0); put(image,100,0xd0000000U); put(image,104,0x87); put(image,108,0xc7);
            put(image,112,1); // Invalid next mask terminates the bounded scan.
            auto program=nt::analysis::discover(image,fixture,64,{},true);
            auto records=nt::analysis::layout_candidates(image,fixture,program);
            check(records.size()==1 && records[0].source==0x2020 && records[0].fields[1]==0xd0000000U,
                "Native layout candidates lost pointer indirection");
            put(image,64,0x2058); // Truncated descriptor is not accepted.
            check(nt::analysis::layout_candidates(image,fixture,program).empty(),"Truncated layout accepted");
            put(image,64,0x2020); program.instructions.at(0x1030)=nt::r5900::decode(0x1030,0x24030055);
            check(nt::analysis::layout_candidates(image,fixture,program).empty(),"Unrelated service treated as layout declaration");
        }
        {
            nt::Bytes image(64,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4}}};
            put(image,0,0x03e00008); // Wrapper is unreachable from the entry.
            put(image,32,0x24030055); put(image,36,12); put(image,40,0x03e00008);
            check(nt::analysis::syscall_wrappers(image,fixture)==std::set<std::uint32_t>{0x1020},
                "Batch wrapper scan lost an unreachable syscall");
            fixture.segments[0].flags=4;
            check(nt::analysis::syscall_wrappers(image,fixture).empty(),"Data segment scanned as wrappers");
            fixture.segments[0].flags=5; put(image,44,12);
            check(nt::analysis::syscall_wrappers(image,fixture).empty(),"Malformed leaf wrapper accepted");
            fixture.segments[0].file_size=68;
            bool rejected=false;
            try { nt::analysis::syscall_wrappers(image,fixture); } catch (const std::invalid_argument&) { rejected=true; }
            check(rejected,"Out-of-file wrapper scan accepted");
        }
        {
            nt::Bytes image(20,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,20,20,5,4}}};
            put(image,0,0x2403007b); put(image,4,12);
            put(image,8,0x03e00008);
            auto program=nt::analysis::discover(image,fixture,64,{},true);
            check(program.instructions.contains(0x1008) && program.instructions.contains(0x100c) &&
                !program.instructions.contains(0x1010),"Leaf syscall return epilogue missing or fell through");
            put(image,12,12);
            program=nt::analysis::discover(image,fixture,64,{},true);
            check(!program.instructions.contains(0x1008),"Unrecognized syscall epilogue accepted");
        }
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
        check(p.instructions.size()==4 && !p.instructions.contains(0x1008),
              "Always-taken branch discovered unreachable data");
        const std::array<std::uint32_t,1> data_entry{0x1008};
        p=nt::analysis::discover(bytes,elf,32,data_entry);
        check(p.instructions.contains(0x1008) && p.instructions.at(0x1008).op==nt::r5900::Op::unsupported,
              "Pruned branch hid an independently reachable unsupported instruction");
        put(bytes,0,0x11090003); // BEQ t0,t1,1010: both outcomes remain possible.
        p = nt::analysis::discover(bytes,elf);
        check(p.instructions.contains(0x1008), "Conditional fallthrough was not discovered");
        for (const unsigned opcode:{4U,20U}) for (const unsigned reg:{0U,8U}) {
            nt::Bytes loop(8,0);
            nt::elf::Executable end_of_code{0x1000,0,{{0,0x1000,8,8,5,4}}};
            put(loop,0,(opcode<<26)|(reg<<21)|(reg<<16)|0xffffU);
            put(loop,4,0x25080001); // ADDIU t0,t0,1 in the executed delay slot.
            const auto discovered=nt::analysis::discover(loop,end_of_code);
            check(discovered.instructions.size()==2 && discovered.unresolved.empty(),
                  "BEQ/BEQL at the end of code invented a fallthrough target");
            check(discovered.instructions.contains(0x1004) && !discovered.ordinary.contains(0x1004),
                  "Always-taken branch lost delay-slot ownership");
            put(loop,0,(opcode<<26)|(reg<<21)|(reg<<16)|3U); // Real target is outside the image.
            const auto invalid_target=nt::analysis::discover(loop,end_of_code);
            check(invalid_target.unresolved==std::set<std::uint32_t>{0x1010},
                  "Always-taken branch lost its actual unmapped target");
        }
        put(bytes,0,0x08000800); // J 2000 outside image
        p = nt::analysis::discover(bytes,elf);
        check(p.unresolved.contains(0x2000) && p.instructions.size() == 2, "Unmapped target lost");
        {
            nt::Bytes image(64,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,64,64,5,4}}};
            put(image,0,0x3c020000); // LUI v0,0
            put(image,4,0x24420000); // ADDIU v0,v0,0
            put(image,8,0x10400003); // BEQ v0,zero,1018
            put(image,16,0x0c000000); // Guarded call to an absent weak symbol.
            put(image,24,0x03e00008);
            auto guarded=nt::analysis::discover(image,fixture);
            check(guarded.unresolved.empty() && !guarded.instructions.contains(0x1010),
                  "Constant weak-symbol guard retained its unreachable null call");
            const std::array<std::uint32_t,1> independent{0x1010};
            guarded=nt::analysis::discover(image,fixture,64,independent);
            check(guarded.unresolved.contains(0),"Constant guard hid an independently reachable null call");
            put(image,0,0x8c020000); // An initial memory value cannot prove the runtime load.
            guarded=nt::analysis::discover(image,fixture);
            check(guarded.unresolved.contains(0),"Mutable memory was treated as a constant branch condition");
            put(image,0,0x3c028000); // LUI v0,8000 -> ffffffff80000000.
            put(image,4,0x6442ffff); // DADDIU -1 -> ffffffff7fffffff, not 000000007fffffff.
            put(image,8,0x3c037fff);
            put(image,12,0x3463ffff); // ORI v1,v1,ffff -> 000000007fffffff.
            put(image,16,0x10430003); // BEQ v0,v1,1020; low words equal, full registers differ.
            put(image,24,0x0c000000);
            put(image,32,0x03e00008);
            guarded=nt::analysis::discover(image,fixture);
            check(guarded.unresolved.contains(0),"Low-word equality incorrectly pruned a 64-bit comparison");
        }
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
            put(image,8,0x0c000410); put(image,12,0xac880010);
            put(image,16,0x8c990010); put(image,20,0x0320f809); put(image,28,12);
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
            put(image,0,0x24081060); put(image,12,0); put(image,16,0xac880010);
            put(image,20,0x8c990010); put(image,24,0x0320f809); put(image,28,0); put(image,32,12);
            check(roots().empty(),"Volatile pointer propagated across call");
            put(image,4,0x10850003); put(image,8,0); put(image,12,0x0c000410); put(image,16,0xac880010);
            check(roots()==std::set<std::uint32_t>{0x1060},"Conditional delay-slot callback store lost");
            put(image,8,0x8ca80000);
            check(roots().empty(),"Conditional store crossed a dynamic register overwrite");
        }
        {
            nt::Bytes image(0x100,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x80,0x80,5,4},{0x80,0x2000,0x80,0x80,6,4}}};
            put(image,0,0x0c000410); put(image,4,0x24051070);
            put(image,8,0x8c990000); put(image,12,0x0320f809); put(image,20,12);
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
            put(image,0x44,0x0c000418); put(image,0x48,0x0080802d);
            put(image,0x4c,0xae170000); put(image,0x60,0x03e00008);
            check(roots()==std::set<std::uint32_t>{0x1070},"Preserved argument copy lost across helper call");
            put(image,0x40,0x0160402d); put(image,0x4c,0xae080000);
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
            for (const auto word:{0x8c850004U,0x8c450008U,0xdc850004U,0x94850004U}) {
                put(image,16,word);
                check(roots()==std::set<std::uint32_t>{0x1060},"Argument load hid object table dispatch");
            }
            put(image,16,0x8c820004);
            check(roots().empty(),"Argument load clobbering the table base was ignored");
            put(image,16,0x84450000);
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
            // The base is hoisted before the range check, rather than rebuilt
            // between the guard and dispatch. Only the bounded table is code.
            put(image,0,0x24032000); put(image,4,0x2c850002);
            put(image,8,0x10a00009); put(image,12,0);
            put(image,16,0x00041080); put(image,20,0x00621021);
            put(image,24,0x8c420000); put(image,28,0x00400008); put(image,48,12);
            put(image,0x60,0x03e00008); put(image,0x70,0x03e00008);
            put(image,0x80,0x1060); put(image,0x84,0x1070);
            const auto roots=[&] {
                return nt::analysis::bounded_switch_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"Hoisted switch base lost");
            put(image,12,0x8c830000);
            check(roots().empty(),"Overwritten hoisted switch base reused");
            put(image,12,0x24840001);
            check(roots().empty(),"Index overwritten after range guard accepted");
            put(image,12,0); put(image,0x84,0x2000);
            check(roots().empty(),"Hoisted switch treated data as executable");
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
            put(image,16,0x24422000);
            for (unsigned at=28;at>=8;at-=4) {
                std::uint32_t word=0; std::memcpy(&word,image.data()+at,4); put(image,at+4,word);
            }
            put(image,8,0x00a03021); // Independent argument move in the delay slot.
            check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"Separate branch-delay move hid switch table");
            put(image,8,0x00a02021);
            check(roots().empty(),"Delay-slot index clobber was ignored");
            put(image,8,0); check(roots()==std::set<std::uint32_t>{0x1060,0x1070},"NOP delay slot hid switch table");
            put(image,0x84,0x1071);
            check(roots().empty(),"Split switch accepted a malformed entry");
        }
        {
            nt::Bytes image(0x120,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x100,0x100,5,4},{0x100,0x2000,0x20,0x20,6,4}}};
            put(image,0x00,0x27bdffe0); // Frame with a table address spilled before a branch.
            put(image,0x04,0x24082000);
            put(image,0x08,0xafa80010);
            put(image,0x0c,0x03a04825); // Copied SP alias.
            put(image,0x10,0x10000003);
            put(image,0x20,0x2c850002);
            put(image,0x24,0x10a00016);
            put(image,0x2c,0x00041080);
            put(image,0x30,0x8d230010); // Reload table base through the alias.
            put(image,0x34,0x00621021);
            put(image,0x38,0x8c420000);
            put(image,0x3c,0x00400008);
            for (const auto at:{0x80U,0x90U,0xa0U}) {
                put(image,at,0x03e00008); put(image,at+4,0x27bd0020);
            }
            put(image,0x100,0x1080); put(image,0x104,0x1090);
            const auto roots=[&] {
                return nt::analysis::bounded_switch_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true));
            };
            check(roots()==std::set<std::uint32_t>{0x1080,0x1090},"Spilled switch table lost across a branch");
            put(image,0x14,0xa1200011); // Overlapping byte write in the branch delay slot.
            check(roots().empty(),"Partial overwrite resurrected a stale switch table");
            put(image,0x14,0xad200010); // A full overwrite must not fall back to the initial spill.
            check(roots().empty(),"Full overwrite resurrected a stale switch table");
            put(image,0x14,0xad200014); // Adjacent data is independent.
            check(roots()==std::set<std::uint32_t>{0x1080,0x1090},"Adjacent stack write hid a switch table");
            put(image,0x104,0x2008);
            check(roots().empty(),"Spilled table promoted a data target to executable code");
            put(image,0x104,0x1090);
            put(image,0x2c,0x8d230010); // Reload before scale instead of after it.
            put(image,0x30,0x00041080);
            check(roots()==std::set<std::uint32_t>{0x1080,0x1090},"Reload-before-scale switch table lost");
            put(image,0x14,0xa1200011);
            check(roots().empty(),"Reload-before-scale resurrected an overwritten spill");
            put(image,0x14,0xad200014);
            put(image,0x28,0x24840001);
            check(roots().empty(),"Reload-before-scale ignored delay-slot index modification");

        }
        {
            nt::Bytes image(0x120,0);
            nt::elf::Executable fixture{0x1000,0,{{0,0x1000,0x100,0x100,5,4},{0x100,0x2000,0x20,0x20,6,4}}};
            const std::uint32_t setup[]{0x27bdffe0,0x24082000,0xafa80010,0x2c850002,
                0x240a0002,0x254affff,0x1540fffe,0,0x10c00003,0,0x10000003,0,0,0,
                0x10a00019,0,0x8fa30010,0x00041080,0x00621021,0x8c420000,0x00400008,0};
            for (unsigned n=0;n<std::size(setup);++n) put(image,n*4,setup[n]);
            for (const auto at:{0x80U,0x90U,0xa0U}) { put(image,at,0x03e00008); put(image,at+4,0x27bd0020); }
            put(image,0x100,0x1080); put(image,0x104,0x1090);
            const auto roots=[&] { return nt::analysis::bounded_switch_entries(image,fixture,nt::analysis::discover(image,fixture,128,{},true)); };
            check(roots()==std::set<std::uint32_t>{0x1080,0x1090},"Hoisted predicate lost across loop and diamond");
            put(image,0x30,0x24840001);
            check(roots().empty(),"One incoming path changed the guarded index");
            put(image,0x30,0x2c850003);
            check(roots().empty(),"Mutually exclusive range predicates were combined");
            put(image,0x30,0); put(image,0x2c,0x24050001);
            check(roots().empty(),"Branch-delay predicate overwrite was ignored");
        }
        std::cout << "Control-flow discovery and address-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
