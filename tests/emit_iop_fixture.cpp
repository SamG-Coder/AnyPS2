#include <nt/iop_codegen.hpp>
#include <nt/iop_ir.hpp>
#include "disc_fixture.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <iostream>

int main(int argc,char** argv) {
    try {
        if (argc<2 || argc>4) return 1;
        constexpr std::uint32_t words[]{0x24080003,0x24090000,0x01284821,0x2508ffff,
            0x1500fffd,0x254a0001,0x8c820000,0x00401821,0x00492821,0xac850004,0x03e00008,0x24060007,
            // Independent byte/halfword copy routine, with signed and unsigned loads.
            0x80820000,0x90830000,0x84850002,0x94860002,0xa0820004,0xa4850006,0x03e00008,0,
            // Direct call, preserved return address and bit operations.
            0x03e08021,0x0c000420,0x2404ffff,0x34420080,
            0x0200f821,0x03e00008,0x38420003,0,
            0,0,0,0,
            0x3082000f,0x2c83ffff,0x03e00008,0x00021040,
            // Return whether a0 is negative, nonnegative, nonpositive or positive.
            0x04800002,0x24020001,0x24020000,0x03e00008,0,
            0x04810002,0x24020001,0x24020000,0x03e00008,0,
            0x18800002,0x24020001,0x24020000,0x03e00008,0,
            0x1c800002,0x24020001,0x24020000,0x03e00008,0,
            0x0085102a,0x2883ffff,0x03e00008,0,
            // Shift a0 by a1, preserving the three distinct shift operations.
            0x000417c3,0x00a41804,0x00a43006,0x00a43807,0x03e00008,0,
            // Observe initial HI/LO, signed/unsigned products, and explicit writes.
            0x00005010,0x00005812,0x00850018,0x00001010,0x00001812,
            0x00850019,0x00003010,0x00003812,0x00800011,0x00a00013,
            0x00004010,0x00004812,0x03e00008,0,
            // Preserve the signed result before the unsigned divide replaces HI/LO.
            0x0085001a,0x00001012,0x00001810,0x0085001b,0x00003012,0x00003810,0x03e00008,0,
            // Conditional trap and a delay-slot trap before unreachable invalid code.
            0x14800002,0,0x0000024d,0x03e00008,0x24020001,
            0x10000001,0x000005cd,0xfc000000,
            // A native service return followed by ordinary compiled continuation.
            0x0000000c,0x24420001,0x03e00008,0,
            // Materialize a function address, call it, then restore the caller.
            0x03e08021,0x3c080000,0x350811c0,0x0100f809,
            0x24040007,0x0200f821,0x03e00008,0x24420001,
            0,0,0,0,
            0x24820002,0x03e00008,0x24420003,
            // A proven register jump to a separately discovered return routine.
            0x3c080000,0x350811dc,0x01000008,0x24020008,0x03e00008,0x24420002,
            // Checked ADD/SUB/ADDI, including a discarded destination.
            0x00851020,0x03e00008,0,
            0x00851022,0x03e00008,0,
            0x2082ffff,0x03e00008,0,
            0x00850020,0x03e00008,0};
        nt::elf::IopModule module{0x1000,0x1000,nt::Bytes(sizeof(words)),sizeof(words),{}};
        for (unsigned i=0;i<std::size(words);++i)
            for (unsigned byte=0;byte<4;++byte) module.memory[i*4+byte]=static_cast<std::uint8_t>(words[i]>>(byte*8));
        // These independent routines are reachable only through an export table.
        const auto table=static_cast<unsigned>(module.memory.size());
        module.memory.resize(table+36); module.initialized_size=table+36;
        const auto export_word=[&](unsigned at,std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) module.memory[table+at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        export_word(0,0x41c00000); export_word(8,0x100);
        constexpr char name[]="fixture";
        std::copy(std::begin(name),std::end(name),module.memory.begin()+table+12);
        export_word(20,0x10f0); export_word(24,0x1108); export_word(28,0x1140);
        const auto imports=module.memory.size(); module.memory.resize(imports+124);
        const auto import_word=[&](unsigned at,std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) module.memory[imports+at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        import_word(0,0x41e00000); import_word(8,0x104);
        constexpr char library[]="sysclib";
        std::copy(std::begin(library),std::end(library),module.memory.begin()+imports+12);
        import_word(20,0x03e00008); import_word(24,0x2400000e);
        import_word(28,0x03e00008); import_word(32,0x2400000c);
        import_word(36,0x03e00008); import_word(40,0x2400000d);
        for (unsigned index=0;index<3;++index) {
            const auto at=52+index*24;
            const auto target=module.base+static_cast<std::uint32_t>(imports)+20+index*8;
            import_word(at,0x03e08021); import_word(at+4,0x0c000000|(target>>2));
            import_word(at+12,0x0200f821); import_word(at+16,0x03e00008);
        }
        // Independent native allocation imports, followed by an ordinary caller.
        const auto heap_table=module.memory.size(); module.memory.resize(heap_table+108);
        const auto heap_word=[&](unsigned at,std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) module.memory[heap_table+at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        heap_word(0,0x41e00000); heap_word(8,0x102);
        constexpr char heap_name[]="sysmem";
        std::copy(std::begin(heap_name),std::end(heap_name),module.memory.begin()+heap_table+12);
        for (unsigned ordinal=4;ordinal<=10;++ordinal) {
            heap_word(20+(ordinal-4)*8,0x03e00008); heap_word(24+(ordinal-4)*8,0x24000000|ordinal);
        }
        heap_word(84,0x03e08021); heap_word(88,0x0c000000|((module.base+static_cast<unsigned>(heap_table)+20)>>2));
        heap_word(96,0x0200f821); heap_word(100,0x03e00008);
        // Two incoming paths select distinct native callees at the same JALR.
        const auto choices=module.memory.size(); module.memory.resize(choices+124);
        constexpr std::uint32_t choice_words[]{0x03e08021,0x10800006,0,0x3c080000,0x35081380,
            0x10000004,0,0,0x3c080000,0x35081360,0x0100f809,0x24050007,
            0x0200f821,0x03e00008,0x24420001,0,0x24a20002,0x03e00008,0,0,0,0,0,0,
            0x24a20004,0x03e00008,0,
            0x3c080000,0x35081360,0x0100f809,0x000001cd};
        for (unsigned index=0;index<std::size(choice_words);++index)
            for (unsigned byte=0;byte<4;++byte) module.memory[choices+index*4+byte]=static_cast<std::uint8_t>(choice_words[index]>>(byte*8));
        const auto loader_table=module.memory.size(); module.memory.resize(loader_table+44);
        const auto loader_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[loader_table+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        constexpr char loader_name[]="loadcore";
        loader_word(0,0x41e00000); loader_word(8,0x103);
        std::copy_n(loader_name,8,module.memory.begin()+loader_table+12);
        loader_word(20,0x03e00008); loader_word(24,0x24000006);
        loader_word(28,0x03e00008); loader_word(32,0x24000007);
        module.initialized_size=static_cast<std::uint32_t>(module.memory.size());
        const std::vector<std::uint32_t> entries{0x1030,0x1050,0x1090,0x10a4,0x10b8,0x10cc,0x10e0,0x1160,0x1174,0x1180,0x1190,0x11cc,0x11e4,0x11f0,0x11fc,0x1208,0x126c,0x1284,0x129c,0x12c8,0x12d0,0x12d8,0x12e0,0x12e8,0x12f0,0x12f8,0x1308,0x1320,0x138c,0x13b0,0x13b8};
        std::ostringstream code; nt::iop_codegen::emit_llvm(code,module,entries);
        // The same explicit bindings must work across major-1 minor revisions.
        for (const unsigned minor : {0U,1U,4U,255U}) {
            auto compatible=module; compatible.memory[imports+8]=static_cast<std::uint8_t>(minor);
            std::ostringstream equivalent; nt::iop_codegen::emit_llvm(equivalent,compatible,entries);
            if (equivalent.str()!=code.str()) throw std::runtime_error("Minor ABI revision changed native binding");
        }
        auto wrong_major=module; wrong_major.memory[imports+9]=2;
        std::ostringstream wrong_output; bool major_failed=false;
        try { nt::iop_codegen::emit_llvm(wrong_output,wrong_major,entries); }
        catch (const std::runtime_error& error) { major_failed=std::string(error.what()).find("version=2.4")!=std::string::npos; }
        if (!major_failed || !wrong_output.str().empty()) throw std::runtime_error("Incompatible major ABI accepted");
        auto unsupported_import=module; unsupported_import.memory[imports+24]=99;
        unsupported_import.memory[imports+32]=98;
        std::ostringstream rejected_import; bool import_failed=false;
        try { nt::iop_codegen::emit_llvm(rejected_import,unsupported_import,entries); }
        catch (const std::runtime_error& error) { const std::string message=error.what();
            import_failed=message.find("unsupported native imports")!=std::string::npos &&
                message.find("ordinal=99")!=std::string::npos && message.find("ordinal=98")!=std::string::npos &&
                message.find("version=1.4")!=std::string::npos; }
        if (!import_failed || !rejected_import.str().empty()) throw std::runtime_error("Unknown native import silently bound");
        auto unsupported=module; unsupported.memory[3]=0xfc;
        std::ostringstream rejected; bool failed=false;
        try { nt::iop_codegen::emit_llvm(rejected,unsupported); }
        catch (const std::runtime_error&) { failed=true; }
        if (!failed || !rejected.str().empty()) throw std::runtime_error("Unsupported code emitted partial LLVM IR");
        auto invalid_export=module;
        invalid_export.memory[table+23]=0x7f;
        std::ostringstream bad_export; bool invalid=false;
        try { nt::iop_codegen::emit_llvm(bad_export,invalid_export,entries); }
        catch (const std::runtime_error&) { invalid=true; }
        if (!invalid || !bad_export.str().empty()) throw std::runtime_error("Invalid export emitted LLVM output");
        for (const bool loaded : {false,true}) {
            auto unknown=module;
            std::vector<std::uint32_t> roots(std::begin(entries),std::end(entries));
            if (loaded) {
                constexpr std::uint32_t load=0x8c880000;
                for (unsigned byte=0;byte<4;++byte) unknown.memory[0x198+byte]=static_cast<std::uint8_t>(load>>(8*byte));
            } else roots.push_back(0x119c); // Another incoming path has unknown registers.
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,unknown,roots); }
            catch (const std::runtime_error& error) { unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Indirect target proof ignored an unknown incoming value");
        }
        for (const auto word : {0x01000008U,0x0100f809U}) {
            auto indirect=module;
            for (unsigned byte=0;byte<4;++byte) indirect.memory[byte]=static_cast<std::uint8_t>(word>>(8*byte));
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,indirect); }
            catch (const std::runtime_error& error) {
                unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos;
            }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Unresolved indirect flow accepted");
        }
        // Unknown calls still expose their delay slots and return continuations.
        // Both backends must report every reachable unknown call without output.
        {
            for (unsigned variant=0;variant<3;++variant) {
                auto callbacks=module; callbacks.memory.assign(24,0); callbacks.initialized_size=24;
                fixture::put32(callbacks.memory,0,0x0100f809); // jalr t0
                fixture::put32(callbacks.memory,8,0x0120f809); // jalr t1
                fixture::put32(callbacks.memory,16,0x03e00008);
                if (variant==1) fixture::put32(callbacks.memory,4,0x08000404); // Invalid branch in delay slot.
                if (variant==2) fixture::put32(callbacks.memory,8,0xfc000000); // Unsupported continuation.
                std::ostringstream partial;
                std::string diagnostic;
                try {
                    nt::iop_codegen::emit_llvm(partial,callbacks);
                } catch (const std::runtime_error& error) { diagnostic=error.what(); }
                const bool expected=variant==0 ?
                    diagnostic.find("0x00001000 through r8")!=std::string::npos &&
                    diagnostic.find("0x00001008 through r9")!=std::string::npos : variant==1 ?
                    diagnostic.find("branch in delay slot at 0x00001004")!=std::string::npos :
                    diagnostic.find("0x00001008")!=std::string::npos && diagnostic.find("unsupported instruction")!=std::string::npos;
                if (!expected || !partial.str().empty())
                    throw std::runtime_error("Unresolved call hid reachable code or emitted a partial program: "+diagnostic);
            }
        }
        // An unbounded pointer-producing loop must widen to unknown and fail,
        // even when every destination seen before widening is executable.
        auto growing=module; growing.memory.assign(0x900,0); growing.initialized_size=0x900;
        const auto grow_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) growing.memory[at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        grow_word(0,0x24081600); grow_word(4,0x25080008); grow_word(8,0x1480fffe);
        grow_word(16,0x0100f809); grow_word(24,0x03e00008);
        for (unsigned offset=0x600;offset<0x900;offset+=8) grow_word(offset,0x03e00008);
        std::ostringstream excessive; bool widened=false;
        try { nt::iop_codegen::emit_llvm(excessive,growing); }
        catch (const std::runtime_error& error) { widened=std::string(error.what()).find("16-value analysis bound")!=std::string::npos; }
        if (!widened || !excessive.str().empty()) throw std::runtime_error("Growing indirect target set was silently truncated");
        // Constant target proofs cover all shift forms and the masked shift count.
        for (unsigned operation=0;operation<6;++operation) for (const auto amount : {0U,2U,34U}) {
            auto shifted=module; shifted.memory.assign(0x60,0); shifted.initialized_size=0x60;
            const bool left=operation==0 || operation==3,variable=operation>=3;
            const unsigned shift=amount&31;
            const std::uint32_t source=left ? (0x1040U>>shift) : (0x1040U<<shift);
            fixture::put32(shifted.memory,0,0x3c080000U|(source>>16));
            fixture::put32(shifted.memory,4,0x35080000U|(source&65535));
            fixture::put32(shifted.memory,8,0x24090000U|amount);
            const unsigned function=operation%3==0 ? 0 : operation%3==1 ? 2 : 3;
            fixture::put32(shifted.memory,12,variable ? (9U<<21)|(8U<<16)|(8U<<11)|(function+4) :
                                                      (8U<<16)|(8U<<11)|(shift<<6)|function);
            fixture::put32(shifted.memory,16,0x01000008); fixture::put32(shifted.memory,0x40,0x03e00008);
            std::ostringstream proof; nt::iop_codegen::emit_llvm(proof,shifted);
            if (proof.str().empty()) throw std::runtime_error("Shifted constant target was not discovered");
            // A mutable load cannot become a target proof through a shift.
            fixture::put32(shifted.memory,0,0x8c880000); fixture::put32(shifted.memory,4,0);
            std::ostringstream rejected_shift; bool unknown=false;
            try { nt::iop_codegen::emit_llvm(rejected_shift,shifted); }
            catch (const std::runtime_error& error) { unknown=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unknown || !rejected_shift.str().empty()) throw std::runtime_error("Shift inferred a target from mutable memory");
        }
        // A non-constant store through one fixed slot is an open indirect.
        // The executable initial word is not a proved target and is not enqueued.
        {
            nt::elf::IopModule unknown_slot{0x40000,0x40000,nt::Bytes(0x200),0x200,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(unknown_slot.memory,pc-unknown_slot.base,word); };
            put(0x40000,0x3c010004); put(0x40004,0xac240180); put(0x40008,0x03e00008);
            put(0x40010,0x3c020004); put(0x40014,0x8c420180); put(0x40018,0);
            put(0x4001c,0x14400003); put(0x40020,0); put(0x40024,0x0801000f); put(0x40028,0x24020001);
            put(0x4002c,0x0040f809); put(0x40030,0); put(0x40034,0x03e00008); put(0x40038,0);
            put(0x4003c,0x03e00008); put(0x40040,0);
            put(0x40100,0x03e00008); put(0x40104,0x24020021); put(0x40180,0x00040100);
            const std::array unknown_roots{0x40010U};
            const auto program=nt::iop_codegen::ir::discover(unknown_slot,unknown_roots);
            const auto site=program.instructions.find(0x4002c);
            if (site==program.instructions.end() || !site->second.targets.empty() ||
                !program.open_indirect_sites.contains(0x4002c) || program.guarded_callback_sites.contains(0x4002c) ||
                program.instructions.contains(0x40100))
                throw std::runtime_error("Non-constant pointer store was not an open indirect");
            std::ostringstream generated;
            nt::iop_codegen::emit_llvm(generated,unknown_slot,unknown_roots);
            const auto ir=generated.str();
            const auto at=ir.find("code262188:");
            const auto next=at==std::string::npos ? std::string::npos : ir.find("\ncode",at+1);
            const auto block=at==std::string::npos ? std::string{} : ir.substr(at,next==std::string::npos ? std::string::npos : next-at);
            if (ir.empty() || block.find("call i1 @nt_iop_contains_instruction")==std::string::npos || block.find("262400")!=std::string::npos)
                throw std::runtime_error("Open indirect was not guarded by compiled instructions");
        }
        // A branch that skips the load must not keep the other path's delayed slot.
        {
            nt::elf::IopModule skipped{0x50000,0x50000,nt::Bytes(0x90),0x90,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(skipped.memory,pc-skipped.base,word); };
            put(0x50000,0x3c010005); put(0x50004,0x15000002); put(0x50008,0); put(0x5000c,0x8c220080);
            put(0x50010,0); put(0x50014,0x0040f809); put(0x50018,0); put(0x5001c,0x03e00008); put(0x50020,0);
            put(0x50030,0x03e00008); put(0x50034,0x24020001); put(0x50080,0x00050030);
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,skipped); }
            catch (const std::runtime_error& error) {
                const std::string message=error.what();
                unresolved=message.find("unresolved indirect")!=std::string::npos && message.find("0x00050014")!=std::string::npos;
            }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Skipped load kept another path's pointer");
        }
        // Stores decoded in another module poison, open, or widen the same slot.
        {
            const auto caller_image=[&] {
                nt::elf::IopModule caller{0x60000,0x60000,nt::Bytes(0x100),0x100,{}};
                const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(caller.memory,pc-caller.base,word); };
                put(0x60000,0x3c010006); put(0x60004,0x8c220080); put(0x60008,0); put(0x6000c,0x0040f809);
                put(0x60010,0); put(0x60014,0x03e00008); put(0x60018,0);
                put(0x60020,0x03e00008); put(0x60024,0x24020001); put(0x60080,0x00060020);
                return caller;
            };
            const auto expect_unresolved=[&](nt::elf::IopModule writer,const char* failure) {
                const std::array<nt::elf::IopModule,2> modules{caller_image(),std::move(writer)};
                bool unresolved=false;
                try { nt::iop_codegen::ir::discover_bundle(modules,std::array<std::uint32_t,2>{0x60000U,0x61000U}); }
                catch (const std::runtime_error& error) {
                    const std::string message=error.what();
                    unresolved=message.find("unresolved indirect")!=std::string::npos && message.find("0x0006000c")!=std::string::npos;
                }
                if (!unresolved) throw std::runtime_error(failure);
            };
            nt::elf::IopModule partial_writer{0x61000,0x61000,nt::Bytes(0x20),0x20,{}};
            fixture::put32(partial_writer.memory,0,0x3c010006); fixture::put32(partial_writer.memory,4,0xa0200080);
            fixture::put32(partial_writer.memory,8,0x03e00008);
            expect_unresolved(std::move(partial_writer),"Partial store in another module left a loaded pointer bounded");
            nt::elf::IopModule open_writer{0x61000,0x61000,nt::Bytes(0x20),0x20,{}};
            fixture::put32(open_writer.memory,0,0x3c010006); fixture::put32(open_writer.memory,4,0xac240080);
            fixture::put32(open_writer.memory,8,0x03e00008);
            const std::array<nt::elf::IopModule,2> open_modules{caller_image(),std::move(open_writer)};
            const auto opened=nt::iop_codegen::ir::discover_bundle(open_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            if (!opened.open_indirect_sites.contains(0x6000c) || opened.instructions.contains(0x60020) ||
                !opened.instructions.at(0x6000c).targets.empty())
                throw std::runtime_error("Cross-module unbounded store was not an open indirect");
            std::ostringstream opened_ir; nt::iop_codegen::emit_program(opened_ir,opened);
            const auto opened_text=opened_ir.str();
            const auto opened_at=opened_text.find("code"+std::to_string(0x6000c)+":");
            const auto opened_next=opened_at==std::string::npos ? std::string::npos : opened_text.find("\ncode",opened_at+1);
            const auto opened_block=opened_at==std::string::npos ? std::string{} :
                opened_text.substr(opened_at,opened_next==std::string::npos ? std::string::npos : opened_next-opened_at);
            if (opened_block.find("call i1 @nt_iop_contains_instruction")==std::string::npos || opened_block.find(std::to_string(0x60020))!=std::string::npos)
                throw std::runtime_error("Cross-module open indirect kept a proved target");
            nt::elf::IopModule wide_writer{0x61000,0x61000,nt::Bytes(0x40),0x40,{}};
            fixture::put32(wide_writer.memory,0,0x3c010006); fixture::put32(wide_writer.memory,4,0x3c040006);
            fixture::put32(wide_writer.memory,8,0x34841020); fixture::put32(wide_writer.memory,12,0xac240080);
            fixture::put32(wide_writer.memory,16,0x03e00008); fixture::put32(wide_writer.memory,0x20,0x03e00008);
            fixture::put32(wide_writer.memory,0x24,0x24020002);
            const std::array<nt::elf::IopModule,2> wide_modules{caller_image(),std::move(wide_writer)};
            const auto widened=nt::iop_codegen::ir::discover_bundle(wide_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            const auto& widened_site=widened.instructions.at(0x6000c);
            bool saw_caller=false,saw_writer=false;
            for (const auto target : widened_site.targets) {
                if (target==0x60020) saw_caller=true;
                if (target==0x61020) saw_writer=true;
            }
            if (widened.open_indirect_sites.contains(0x6000c) || !saw_caller || !saw_writer ||
                !widened.instructions.contains(0x60020) || !widened.instructions.contains(0x61020))
                throw std::runtime_error("Cross-module executable store did not widen the proved set");
            // The proved callee lives in the writer. The first pass records it before
            // the writer's store is visible; the finished bundle must not keep it.
            nt::elf::IopModule retracted_caller{0x60000,0x60000,nt::Bytes(0x100),0x100,{}};
            const auto put_caller=[&](unsigned pc,std::uint32_t word) { fixture::put32(retracted_caller.memory,pc-retracted_caller.base,word); };
            put_caller(0x60000,0x3c010006); put_caller(0x60004,0x8c220080); put_caller(0x60008,0); put_caller(0x6000c,0x0040f809);
            put_caller(0x60010,0); put_caller(0x60014,0x03e00008); put_caller(0x60018,0); put_caller(0x60080,0x00061020);
            nt::elf::IopModule retracted_writer{0x61000,0x61000,nt::Bytes(0x40),0x40,{}};
            fixture::put32(retracted_writer.memory,0,0x3c010006); fixture::put32(retracted_writer.memory,4,0xac240080);
            fixture::put32(retracted_writer.memory,8,0x03e00008); fixture::put32(retracted_writer.memory,0x20,0x03e00008);
            fixture::put32(retracted_writer.memory,0x24,0x24020002);
            const std::array<nt::elf::IopModule,2> retracted_modules{std::move(retracted_caller),std::move(retracted_writer)};
            const auto retracted=nt::iop_codegen::ir::discover_bundle(retracted_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            if (!retracted.open_indirect_sites.contains(0x6000c) || retracted.instructions.contains(0x61020) ||
                !retracted.instructions.contains(0x61000) || !retracted.instructions.at(0x6000c).targets.empty())
                throw std::runtime_error("Retracted writer leaf stayed compiled");
            std::ostringstream retracted_ir; nt::iop_codegen::emit_program(retracted_ir,retracted);
            const auto retracted_text=retracted_ir.str();
            const auto retracted_at=retracted_text.find("code"+std::to_string(0x6000c)+":");
            const auto retracted_next=retracted_at==std::string::npos ? std::string::npos : retracted_text.find("\ncode",retracted_at+1);
            const auto retracted_block=retracted_at==std::string::npos ? std::string{} :
                retracted_text.substr(retracted_at,retracted_next==std::string::npos ? std::string::npos : retracted_next-retracted_at);
            if (retracted_block.find("call i1 @nt_iop_contains_instruction")==std::string::npos || retracted_text.find("code"+std::to_string(0x61020)+":")!=std::string::npos)
                throw std::runtime_error("Retracted writer leaf remained a compiled indirect target");
            // The writer's destination is an argument, so it can be the caller's slot.
            // The containing module is not a proof that the slot stays unchanged.
            nt::elf::IopModule alias_writer{0x61000,0x61000,nt::Bytes(0x20),0x20,{}};
            fixture::put32(alias_writer.memory,0,0xac850000);
            fixture::put32(alias_writer.memory,4,0x03e00008);
            const std::array<nt::elf::IopModule,2> alias_modules{caller_image(),std::move(alias_writer)};
            const auto aliased=nt::iop_codegen::ir::discover_bundle(alias_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            if (!aliased.open_indirect_sites.contains(0x6000c) || aliased.instructions.contains(0x60020) ||
                !aliased.instructions.contains(0x61000) || !aliased.instructions.at(0x6000c).targets.empty())
                throw std::runtime_error("Unknown store in another module left a loaded pointer proved");
            std::ostringstream alias_ir; nt::iop_codegen::emit_program(alias_ir,aliased);
            const auto alias_text=alias_ir.str();
            const auto alias_at=alias_text.find("code"+std::to_string(0x6000c)+":");
            const auto alias_next=alias_at==std::string::npos ? std::string::npos : alias_text.find("\ncode",alias_at+1);
            const auto alias_block=alias_at==std::string::npos ? std::string{} :
                alias_text.substr(alias_at,alias_next==std::string::npos ? std::string::npos : alias_next-alias_at);
            if (alias_block.find("call i1 @nt_iop_contains_instruction")==std::string::npos || alias_text.find("code"+std::to_string(0x60020)+":")!=std::string::npos)
                throw std::runtime_error("Cross-module unknown store kept a proved loaded pointer");
            // A stack spill has an address range that cannot name the absolute slot.
            nt::elf::IopModule stack_writer{0x61000,0x61000,nt::Bytes(0x20),0x20,{}};
            fixture::put32(stack_writer.memory,0,0x27bdfff0);
            fixture::put32(stack_writer.memory,4,0xafa40000);
            fixture::put32(stack_writer.memory,8,0x27bd0010);
            fixture::put32(stack_writer.memory,12,0x03e00008);
            const std::array<nt::elf::IopModule,2> stack_modules{caller_image(),std::move(stack_writer)};
            const auto stacked=nt::iop_codegen::ir::discover_bundle(stack_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            if (stacked.open_indirect_sites.contains(0x6000c) || !stacked.instructions.contains(0x60020) ||
                stacked.instructions.at(0x6000c).targets.size()!=1 || stacked.instructions.at(0x6000c).targets.front()!=0x60020)
                throw std::runtime_error("Stack spill in another module dropped a proved loaded pointer");
            // The unknown store is only inside the proved leaf. Removing that leaf
            // must not bring the proof back.
            nt::elf::IopModule oscillating_caller{0x60000,0x60000,nt::Bytes(0x100),0x100,{}};
            const auto put_oscillating=[&](unsigned pc,std::uint32_t word) {
                fixture::put32(oscillating_caller.memory,pc-oscillating_caller.base,word);
            };
            put_oscillating(0x60000,0x3c010006); put_oscillating(0x60004,0x8c220080); put_oscillating(0x60008,0);
            put_oscillating(0x6000c,0x0040f809); put_oscillating(0x60010,0); put_oscillating(0x60014,0x03e00008);
            put_oscillating(0x60018,0); put_oscillating(0x60080,0x00061020);
            nt::elf::IopModule oscillating_writer{0x61000,0x61000,nt::Bytes(0x40),0x40,{}};
            fixture::put32(oscillating_writer.memory,0,0x03e00008);
            fixture::put32(oscillating_writer.memory,0x20,0xac850000);
            fixture::put32(oscillating_writer.memory,0x24,0x03e00008);
            const std::array<nt::elf::IopModule,2> oscillating_modules{std::move(oscillating_caller),std::move(oscillating_writer)};
            const auto settled=nt::iop_codegen::ir::discover_bundle(oscillating_modules,std::array<std::uint32_t,2>{0x60000U,0x61000U});
            if (!settled.open_indirect_sites.contains(0x6000c) || settled.instructions.contains(0x61020) ||
                !settled.instructions.contains(0x61000) || !settled.instructions.at(0x6000c).targets.empty())
                throw std::runtime_error("Retracted unknown store let the loaded pointer become proved again");
        }
        // The only unbounded store is inside the initial-word leaf. Opening the
        // slot must remove that leaf instead of leaving the first proof compiled.
        {
            nt::elf::IopModule image{0x80000,0x80000,nt::Bytes(0x90),0x90,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(image.memory,pc-image.base,word); };
            put(0x80000,0x3c010008); put(0x80004,0x8c220080); put(0x80008,0); put(0x8000c,0x0040f809);
            put(0x80010,0); put(0x80014,0x03e00008); put(0x80018,0);
            put(0x80040,0x3c010008); put(0x80044,0xac240080); put(0x80048,0x03e00008); put(0x8004c,0);
            put(0x80080,0x00080040);
            const auto opened=nt::iop_codegen::ir::discover(image);
            if (!opened.open_indirect_sites.contains(0x8000c) || opened.instructions.contains(0x80040) ||
                !opened.instructions.at(0x8000c).targets.empty())
                throw std::runtime_error("Retracted same-module leaf stayed compiled");
            std::ostringstream generated; nt::iop_codegen::emit_llvm(generated,image);
            const auto ir=generated.str();
            const auto at=ir.find("code"+std::to_string(0x8000c)+":");
            const auto next=at==std::string::npos ? std::string::npos : ir.find("\ncode",at+1);
            const auto block=at==std::string::npos ? std::string{} : ir.substr(at,next==std::string::npos ? std::string::npos : next-at);
            if (block.find("call i1 @nt_iop_contains_instruction")==std::string::npos || ir.find("code"+std::to_string(0x80040)+":")!=std::string::npos)
                throw std::runtime_error("Retracted same-module leaf remained a compiled indirect target");
        }
        // A constant returned from a reached helper is not an instruction edge.
        {
            nt::elf::IopModule image{0x90000,0x90000,nt::Bytes(0x50),0x50,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(image.memory,pc-image.base,word); };
            put(0x90000,0x0c024008); put(0x90004,0); put(0x90008,0x03e00008); put(0x9000c,0);
            put(0x90020,0x3c020009); put(0x90024,0x34420040); put(0x90028,0x03e00008); put(0x9002c,0);
            put(0x90040,0x03e00008); put(0x90044,0x24020007);
            const auto program=nt::iop_codegen::ir::discover(image);
            if (!program.instructions.contains(0x90040) || !program.instructions.contains(0x90020))
                throw std::runtime_error("Returned constant callback was dropped");
        }
        // A constant memset poisons its span. It does not drop every other slot.
        {
            nt::elf::IopModule filled{0x70000,0x70000,nt::Bytes(0x140),0x140,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(filled.memory,pc-filled.base,word); };
            put(0x70000,0x3c040007); put(0x70004,0x34840040); put(0x70008,0x24050000); put(0x7000c,0x24060010);
            put(0x70010,0x0c01c045); put(0x70014,0); put(0x70018,0x3c010007); put(0x7001c,0x8c220080);
            put(0x70020,0); put(0x70024,0x0040f809); put(0x70028,0); put(0x7002c,0x03e00008); put(0x70030,0);
            put(0x70060,0x03e00008); put(0x70064,0x24020001); put(0x70080,0x00070060);
            put(0x70100,0x41e00000); put(0x70104,0); put(0x70108,0x00000104);
            put(0x7010c,0x63737973); put(0x70110,0x0062696c); put(0x70114,0x03e00008); put(0x70118,0x2400000e);
            const auto proved=nt::iop_codegen::ir::discover(filled);
            const auto& site=proved.instructions.at(0x70024);
            if (site.targets.size()!=1 || site.targets.front()!=0x70060 || proved.open_indirect_sites.contains(0x70024))
                throw std::runtime_error("Constant memset dropped an unrelated loaded pointer");
            auto overlap=filled; fixture::put32(overlap.memory,0xc,0x24060080);
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,overlap); }
            catch (const std::runtime_error& error) { unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Overlapping memset left a loaded pointer bounded");
            auto unknown=filled; fixture::put32(unknown.memory,0,0); fixture::put32(unknown.memory,4,0);
            const auto opened_fill=nt::iop_codegen::ir::discover(unknown);
            if (!opened_fill.open_indirect_sites.contains(0x70024) || opened_fill.instructions.contains(0x70060) ||
                !opened_fill.instructions.at(0x70024).targets.empty())
                throw std::runtime_error("Unknown memset left a single-slot load unresolved");
        }
        // A call between the load and the use discards the loaded register.
        {
            nt::elf::IopModule clobber{0x20000,0x20000,nt::Bytes(0x40),0x40,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(clobber.memory,pc-clobber.base,word); };
            put(0x20000,0x3c020002); put(0x20004,0x8c420030); put(0x20008,0); put(0x2000c,0x0c008009); put(0x20010,0);
            put(0x20014,0x0040f809); put(0x20018,0); put(0x2001c,0x03e00008); put(0x20020,0);
            put(0x20024,0x03e00008); put(0x20028,0); put(0x20030,0x00020024);
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,clobber); }
            catch (const std::runtime_error& error) { unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Call preserved a loaded pointer register");
        }
        // A shared thread factory sees different caller stack depths. Its local
        // descriptor must still retain the callback as a compilation candidate.
        {
            nt::elf::IopModule image{0xC0000,0xC0000,nt::Bytes(0x100),0x100,{}};
            const auto put=[&](unsigned offset,std::uint32_t word) { fixture::put32(image.memory,offset,word); };
            put(0,0x0080e821); // caller supplies an unknown stack origin
            put(4,0x0c030010); put(8,0);
            put(12,0x27bdffe0); put(16,0x0c030010); put(20,0); // different stack
            put(24,0x03e00008); put(28,0);
            put(0x40,0x27bdffd0); put(0x44,0x27a40010);
            put(0x48,0x3c02000c); put(0x4c,0x244200e0);
            put(0x50,0xafa20018); put(0x54,0x0c030025); put(0x58,0);
            put(0x5c,0x03e00008); put(0x60,0);
            put(0x80,0x41e00000); put(0x84,0); put(0x88,0x00000101);
            put(0x8c,0x61626874); put(0x90,0x00006573);
            put(0x94,0x03e00008); put(0x98,0x24000004);
            put(0xe0,0x03e00008); put(0xe4,0x24020009);
            const auto program=nt::iop_codegen::ir::discover(image);
            if (!program.instructions.contains(0xC00E0))
                throw std::runtime_error("Merged caller stacks hid a local thread descriptor");
        }
        // Indexed callback registries use relocation candidates, not a claimed
        // immutable table. An unrelated runtime address must still be guarded.
        {
            nt::elf::IopModule registry{0xD0000,0xD0000,nt::Bytes(0x80),0x80,{}};
            const auto put=[&](unsigned offset,std::uint32_t word) { fixture::put32(registry.memory,offset,word); };
            put(0,0x00851021); put(4,0x8c420000); put(8,0);
            put(12,0x0040f809); put(16,0); put(20,0x03e00008); put(24,0);
            put(0x60,0x03e00008); put(0x64,0x24020003);
            registry.code_addresses={0xD0060};
            const auto program=nt::iop_codegen::ir::discover(registry);
            if (!program.instructions.contains(0xD0060) || program.instructions.at(0xD000c).targets!=registry.code_addresses ||
                !program.guarded_callback_sites.contains(0xD000c))
                throw std::runtime_error("Indexed callback relocation candidates were not guarded");
        }
        // A host import keeps a descriptor that lives in a saved register.
        // The following AddDrv receives it from the delay slot.
        {
            nt::elf::IopModule saved{0xA0000,0xA0000,nt::Bytes(0x100),0x100,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(saved.memory,pc-saved.base,word); };
            put(0xA0000,0x3c10000a); put(0xA0004,0x26100080);
            put(0xA0008,0x0c028015); put(0xA000c,0);
            put(0xA0010,0x0c028017); put(0xA0014,0x02002021);
            put(0xA0018,0x03e00008); put(0xA001c,0);
            put(0xA0040,0x41e00000); put(0xA0044,0); put(0xA0048,0x00000101);
            put(0xA004c,0x616d6f69); put(0xA0050,0x0000006e);
            put(0xA0054,0x03e00008); put(0xA0058,0x24000015);
            put(0xA005c,0x03e00008); put(0xA0060,0x24000014);
            put(0xA0080,0x000a00a0); put(0xA0084,0x10); put(0xA0090,0x000a00b0);
            put(0xA00a0,0x0000636d);
            put(0xA00b0,0x000a00f0); put(0xA00b4,0x000a00f8);
            put(0xA00f0,0x03e00008); put(0xA00f4,0);
            put(0xA00f8,0x03e00008); put(0xA00fc,0);
            const auto program=nt::iop_codegen::ir::discover(saved);
            if (!program.instructions.contains(0xA00F0) || !program.instructions.contains(0xA00F8))
                throw std::runtime_error("Host import dropped an AddDrv descriptor held in a saved register");
        }
        // A host import discards a pointer held in a caller-saved register.
        {
            nt::elf::IopModule temporary{0xB0000,0xB0000,nt::Bytes(0x80),0x80,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(temporary.memory,pc-temporary.base,word); };
            put(0xB0000,0x3c08000b); put(0xB0004,0x25080060);
            put(0xB0008,0x0c02c00d); put(0xB000c,0); put(0xB0010,0x01000008); put(0xB0014,0);
            put(0xB0020,0x41e00000); put(0xB0024,0); put(0xB0028,0x00000101);
            put(0xB002c,0x616d6f69); put(0xB0030,0x0000006e);
            put(0xB0034,0x03e00008); put(0xB0038,0x24000015);
            put(0xB0060,0x03e00008); put(0xB0064,0);
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,temporary); }
            catch (const std::runtime_error& error) { unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Host import preserved a caller-saved pointer");
        }
        // A partial store overlaps the loaded word and rejects an otherwise valid pointer.
        {
            nt::elf::IopModule partial_store{0x30000,0x30000,nt::Bytes(0x90),0x90,{}};
            const auto put=[&](unsigned pc,std::uint32_t word) { fixture::put32(partial_store.memory,pc-partial_store.base,word); };
            put(0x30000,0x3c010003); put(0x30004,0xa0200080); put(0x30008,0x03e00008);
            put(0x30010,0x3c020003); put(0x30014,0x8c420080); put(0x30018,0); put(0x3001c,0x00400008); put(0x30020,0);
            put(0x30028,0x03e00008); put(0x3002c,0x24020001); put(0x30080,0x00030028);
            const std::array partial_roots{0x30010U};
            std::ostringstream partial; bool unresolved=false;
            try { nt::iop_codegen::emit_llvm(partial,partial_store,partial_roots); }
            catch (const std::runtime_error& error) { unresolved=std::string(error.what()).find("unresolved indirect")!=std::string::npos; }
            if (!unresolved || !partial.str().empty()) throw std::runtime_error("Partial store left a loaded pointer bounded");
        }
        for (const bool variable : {false,true}) {
            auto signed_shift=module; signed_shift.memory.assign(0x60,0); signed_shift.initialized_size=0x60;
            fixture::put32(signed_shift.memory,0,0x3c088000);
            fixture::put32(signed_shift.memory,4,0x2409003f);
            fixture::put32(signed_shift.memory,8,variable ? 0x01284007 : 0x000847c3);
            fixture::put32(signed_shift.memory,12,0x31081040);
            fixture::put32(signed_shift.memory,16,0x01000008);
            fixture::put32(signed_shift.memory,0x40,0x03e00008);
            std::ostringstream proof; nt::iop_codegen::emit_llvm(proof,signed_shift);
            if (proof.str().empty()) throw std::runtime_error("Arithmetic target proof lost sign extension");
        }
        for (const unsigned count : {1024U,1025U}) {
            auto bounded=module; bounded.memory.assign(count*4,0); bounded.initialized_size=count*4;
            const auto at=(count-2)*4;
            bounded.memory[at]=8; bounded.memory[at+2]=0xe0; bounded.memory[at+3]=3;
            std::ostringstream partial; bool limited=false;
            try { nt::iop_codegen::emit_llvm(partial,bounded); }
            catch (const std::runtime_error& error) {
                limited=std::string(error.what()).find("function limit")!=std::string::npos;
                if (!limited) throw;
            }
            if (limited || partial.str().empty())
                throw std::runtime_error("Generated function bound excludes its delay slot");
        }
        // Separately relocated provider and client exercise true bundle imports.
        nt::elf::IopModule provider{0xa000,0xa000,nt::Bytes(64),64,{}};
        const auto provider_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) provider.memory[at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        provider_word(0,0x24820005); provider_word(4,0x03e00008);
        provider_word(32,0x41c00000); provider_word(40,0x100); provider_word(52,0xa000);
        constexpr char worker_name[]="worker";
        std::copy(std::begin(worker_name),std::end(worker_name),provider.memory.begin()+44);
        const auto client_table=module.memory.size(); module.memory.resize(client_table+104);
        const auto client_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[client_table+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        client_word(0,0x41e00000); client_word(8,0x103);
        std::copy(std::begin(worker_name),std::end(worker_name),module.memory.begin()+client_table+12);
        client_word(20,0x03e00008); client_word(24,0x24000000);
        client_word(36,0x03e08021); client_word(40,0x0c0004f7);
        client_word(48,0x0200f821); client_word(52,0x03e00008);
        client_word(60,0x41e00000); client_word(68,0x103);
        std::copy_n(loader_name,8,module.memory.begin()+client_table+72);
        client_word(80,0x03e00008); client_word(84,0x24000008);
        client_word(88,0x03e00008); client_word(92,0x24000009);
        module.memory.resize(0x544);
        const auto query_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[0x500+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        query_word(0,0x41e00000); query_word(8,0x103);
        std::copy_n(loader_name,8,module.memory.begin()+0x50c);
        query_word(20,0x03e00008); query_word(24,0x2400000b);
        query_word(28,0x03e00008); query_word(32,0x2400000c);
        query_word(36,0x03e00008); query_word(40,0x2400001b);
        query_word(44,0x03e00008); query_word(48,0x2400000a);
        query_word(52,0x03e00008); query_word(56,0x24000014);
        module.memory.resize(0x63c);
        const auto file_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[0x600+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        file_word(0,0x41e00000); file_word(8,0x104);
        constexpr char file_name[]="ioman";
        std::copy(std::begin(file_name),std::end(file_name),module.memory.begin()+0x60c);
        const unsigned file_ordinals[]{4,5,6,8};
        for (unsigned index=0;index<4;++index) { file_word(20+index*8,0x03e00008); file_word(24+index*8,0x24000000|file_ordinals[index]); }
        module.memory.resize(0x2000);
        const auto irq_word=[&](unsigned address,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[address-module.base+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        irq_word(0x1700,0x41e00000); irq_word(0x1708,0x102);
        constexpr char irq_name[]="intrman";
        std::copy(std::begin(irq_name),std::end(irq_name),module.memory.begin()+0x70c);
        const unsigned irq_ordinals[]{4,5,6,7,8,9,17,18,23};
        for (unsigned index=0;index<9;++index) { irq_word(0x1714+index*8,0x03e00008); irq_word(0x1718+index*8,0x24000000|irq_ordinals[index]); }
        irq_word(0x175c,0x03e00008); irq_word(0x1760,0x2400000e);
        // Only the native invocation argument discovers this private callback.
        constexpr std::uint32_t shifted_callback[]{0x240405f0,0x00042080,0x24080022,0x01042004,
            0x00042082,0x080005d7,0};
        for (unsigned i=0;i<std::size(shifted_callback);++i) irq_word(0x17a0+i*4,shifted_callback[i]);
        constexpr std::uint32_t invoke_callback[]{0x00a61021,0xac820000,0x03e00008,0};
        for (unsigned i=0;i<std::size(invoke_callback);++i) irq_word(0x17c0+i*4,invoke_callback[i]);
        // Handler is discovered only through the constant registration argument.
        constexpr std::uint32_t handler[]{0x03e08021,0x0c0005d5,0,0xac820004,0x8c880000,0,0x25080001,0xac880000,
            0x24020001,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(handler);++index) irq_word(0x1a00+index*4,handler[index]);
        // Consume stack before calling the stack guard; no system stack is fabricated.
        irq_word(0x1a60,0x27bdfe80); irq_word(0x1a64,0x0800066f); irq_word(0x1a68,0);
        irq_word(0x1a70,0x24042ec0); irq_word(0x1a74,0x0800054d); irq_word(0x1a78,0);
        constexpr std::uint32_t boot_callback[]{0x3408c700,0x8d090000,0,0xad3c0000,0xad250004,
            0x8c8a0000,0,0xad2a0008,0x2529000c,0xad090000,0x03e00008,0x2402fff9};
        for (unsigned i=0;i<std::size(boot_callback);++i) irq_word(0x2ec0+i*4,boot_callback[i]);
        irq_word(0x1a40,0x24020000); irq_word(0x1a44,0x03e00008); irq_word(0x1a50,0x0000008d);
        constexpr std::uint32_t registration[]{0x03e08021,0x24040003,0x24050002,0x24061a00,0x3407c000,
            0x0c0005c5,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(registration);++index) irq_word(0x1b00+index*4,registration[index]);
        irq_word(0x1aac,0x41e00000); irq_word(0x1ab4,0x101);
        constexpr char heap_library_name[]="heaplib";
        std::copy(std::begin(heap_library_name),std::end(heap_library_name),module.memory.begin()+0xab8);
        for (unsigned i=0;i<5;++i) { irq_word(0x1ac0+i*8,0x03e00008); irq_word(0x1ac4+i*8,0x24000004+i); }
        irq_word(0x1800,0x41e00000); irq_word(0x1808,0x101);
        constexpr char event_name[]="thevent";
        std::copy(std::begin(event_name),std::end(event_name),module.memory.begin()+0x80c);
        const unsigned event_ordinals[]{4,5,6,7,8,9,10,11,13,14};
        for (unsigned index=0;index<std::size(event_ordinals);++index) {
            irq_word(0x1814+index*8,0x03e00008); irq_word(0x1818+index*8,0x24000000|event_ordinals[index]);
        }
        constexpr std::uint32_t event_handler[]{0x03e08021,0x24050008,0x0c00060b,0,0x24020001,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(event_handler);++index) irq_word(0x1c00+index*4,event_handler[index]);
        irq_word(0x1900,0x41e00000); irq_word(0x1908,0x102);
        constexpr char thread_name[]="thbase";
        std::copy(std::begin(thread_name),std::end(thread_name),module.memory.begin()+0x90c);
        const unsigned thread_ordinals[]{20,24,25,26,27,28,4,5,6,8,9,7,41,34,35,36,37,38,14,22,33,21,10};
        for (unsigned index=0;index<std::size(thread_ordinals);++index) {
            irq_word(0x1914+index*8,0x03e00008); irq_word(0x1918+index*8,0x24000000|thread_ordinals[index]);
        }
        constexpr std::uint32_t sleeper[]{0x03e08021,0x0c000647,0,0x2402002b,0x0200f821,0x03e00008,0};
        constexpr std::uint32_t wake_handler[]{0x03e08021,0x0c00064b,0,0x24020001,0x0200f821,0x03e00008,0};
        constexpr std::uint32_t wake_thread[]{0x03e08021,0x0c000649,0,0x24020063,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(sleeper);++index) irq_word(0x1d00+index*4,sleeper[index]);
        for (unsigned index=0;index<std::size(wake_handler);++index) irq_word(0x1d80+index*4,wake_handler[index]);
        for (unsigned index=0;index<std::size(wake_thread);++index) irq_word(0x1dc0+index*4,wake_thread[index]);
        // Neither exit routine may reach its trailing BREAK.
        irq_word(0x1e00,0x0c000657); irq_word(0x1e04,0); irq_word(0x1e08,0x0000008d);
        irq_word(0x1e40,0x0c000659); irq_word(0x1e44,0); irq_word(0x1e48,0x0000008d);
        constexpr std::uint32_t argument_worker[]{0x90a20000,0,0x00441021,0x03e00008,0};
        for (unsigned index=0;index<std::size(argument_worker);++index) irq_word(0x1e80+index*4,argument_worker[index]);
        constexpr std::uint32_t alarm_once[]{0x8c880000,0,0x25080001,0xac880000,0x24020000,0x03e00008,0};
        for (unsigned index=0;index<std::size(alarm_once);++index) {
            irq_word(0x2100+index*4,alarm_once[index]); irq_word(0x2140+index*4,alarm_once[index]);
        }
        irq_word(0x2150,0x240203e8); // Return 1000 ticks, clamped to the minimum repeat interval.
        constexpr std::uint32_t alarm_wake[]{0x03e08021,0x0c00064b,0,0x24020000,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(alarm_wake);++index) irq_word(0x2180+index*4,alarm_wake[index]);
        constexpr std::uint32_t alarm_registration[]{0x03e08021,0x3404c700,0x24052100,0x3406c720,0x0c000661,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(alarm_registration);++index) irq_word(0x21c0+index*4,alarm_registration[index]);
        irq_word(0x2200,0x41e00000); irq_word(0x2208,0x101);
        constexpr char sif_name[]="sifman";
        std::copy(std::begin(sif_name),std::end(sif_name),module.memory.begin()+0x120c);
        const unsigned sif_ordinals[]{7,8,30,31,32,5,6,21,22,23,24,25,26,27,29,2};
        for (unsigned index=0;index<std::size(sif_ordinals);++index) {
            irq_word(0x2214+index*8,0x03e00008); irq_word(0x2218+index*8,0x24000000|sif_ordinals[index]);
        }
        irq_word(0x2300,0x41e00000); irq_word(0x2308,0x103);
        constexpr char stdio_name[]="stdio";
        std::copy(std::begin(stdio_name),std::end(stdio_name),module.memory.begin()+0x130c);
        irq_word(0x2314,0x03e00008); irq_word(0x2318,0x24000004);
        irq_word(0x2380,0x41e00000); irq_word(0x2388,0x104);
        constexpr char stat_library[]="ioman";
        std::copy(std::begin(stat_library),std::end(stat_library),module.memory.begin()+0x138c);
        const unsigned directory_ordinals[]{16,13,14,15,7,10,11,12,17,18,30,9};
        for (unsigned index=0;index<std::size(directory_ordinals);++index) {
            irq_word(0x2394+index*8,0x03e00008); irq_word(0x2398+index*8,0x24000000|directory_ordinals[index]);
        }
        irq_word(0x2800,0x41e00000); irq_word(0x2808,0x104);
        std::copy(std::begin(stat_library),std::end(stat_library),module.memory.begin()+0x180c);
        irq_word(0x2814,0x03e00008); irq_word(0x2818,0x24000022);
        irq_word(0x281c,0x03e00008); irq_word(0x2820,0x24000014);
        irq_word(0x2824,0x03e00008); irq_word(0x2828,0x24000015);
        irq_word(0x282c,0x03e00008); irq_word(0x2830,0x2400001f);
        // Lifecycle routines are private: only the AddDrv descriptor discovers them.
        irq_word(0x2840,0x240428c0); irq_word(0x2844,0x08000a07); irq_word(0x2848,0);
        irq_word(0x2860,0x8c820008); irq_word(0x2864,0x03e00008); irq_word(0x2868,0);
        irq_word(0x2880,0x2402ffff); irq_word(0x2884,0x03e00008); irq_word(0x2888,0);
        irq_word(0x28c0,0x28e0); irq_word(0x28c4,0x10); irq_word(0x28d0,0x28f0);
        constexpr char driver_name[]="fixture";
        std::copy(std::begin(driver_name),std::end(driver_name),module.memory.begin()+0x18e0);
        irq_word(0x28f0,0x2860); irq_word(0x28f4,0x2880);
        irq_word(0x28fc,0x2940); irq_word(0x2900,0x2960); irq_word(0x2904,0x2980);
        irq_word(0x2908,0x2a00); irq_word(0x290c,0x2a40);
        irq_word(0x2920,0x2b00); irq_word(0x2924,0x2b80); irq_word(0x2928,0x2b20);
        // Original device: open preserves the relative path as private data;
        // read returns its first byte; close reports an error after cleanup.
        constexpr std::uint32_t driver_open[]{0x8c880008,0,0x8d020008,0,0xac85000c,0x03e00008,0};
        constexpr std::uint32_t driver_close[]{0xac80000c,0x2402fff9,0x03e00008,0};
        constexpr std::uint32_t driver_read[]{0x10c00008,0x8c88000c,0,0x91090000,0,0xa0a90000,0x24020001,0x03e00008,0,0x24020000,0x03e00008,0};
        for (unsigned i=0;i<std::size(driver_open);++i) irq_word(0x2940+i*4,driver_open[i]);
        for (unsigned i=0;i<std::size(driver_close);++i) irq_word(0x2960+i*4,driver_close[i]);
        for (unsigned i=0;i<std::size(driver_read);++i) irq_word(0x2980+i*4,driver_read[i]);
        // Write changes one byte at the private cursor. Seek moves that cursor
        // and returns the signed displacement to expose argument/result wiring.
        constexpr std::uint32_t driver_write[]{0x10c00008,0x8c88000c,0,0x90a90000,0,0xa1090000,0x24020001,0x03e00008,0,0x24020000,0x03e00008,0};
        constexpr std::uint32_t driver_seek[]{0x8c88000c,0,0x01054021,0xac88000c,0x00a01021,0x03e00008,0};
        for (unsigned i=0;i<std::size(driver_write);++i) irq_word(0x2a00+i*4,driver_write[i]);
        for (unsigned i=0;i<std::size(driver_seek);++i) irq_word(0x2a40+i*4,driver_seek[i]);
        constexpr std::uint32_t directory_open[]{0xac80000c,0x03e00008,0x00001021};
        constexpr std::uint32_t directory_read[]{0x8c88000c,0,0x1500000f,0x24020000,0x24080001,0xac88000c,
            0x24090014,0xaca90000,0x24090011,0xaca90008,0x3c097274,0x35296e65,0xaca90028,
            0x24090079,0xa0a9002c,0xa0a0002d,0xaca00128,0x24020001,0x03e00008,0};
        for (unsigned i=0;i<std::size(directory_open);++i) {
            irq_word(0x2b00+i*4,directory_open[i]); irq_word(0x2b80+i*4,directory_open[i]);
        }
        for (unsigned i=0;i<std::size(directory_read);++i) irq_word(0x2b20+i*4,directory_read[i]);
        irq_word(0x2ba0,0x24042bc0); irq_word(0x2ba4,0x08000a07); irq_word(0x2ba8,0);
        irq_word(0x2bc0,0x28e0); irq_word(0x2bc4,0x10000010); irq_word(0x2bd0,0x2c00);
        irq_word(0x2c00,0x2860); irq_word(0x2c04,0x2880); irq_word(0x2c0c,0x2940); irq_word(0x2c10,0x2960);
        irq_word(0x2c48,0x2e00); irq_word(0x2c4c,0x2e40); irq_word(0x2c54,0x2880);
        irq_word(0x2f00,0x41e00000); irq_word(0x2f08,0x104);
        std::copy(std::begin(stat_library),std::end(stat_library),module.memory.begin()+0x1f0c);
        for (const auto [offset,ordinal] : {std::pair{20U,26U},{28U,27U},{36U,29U},{44U,33U},{52U,25U},{60U,32U},{68U,22U},{76U,28U}}) {
            irq_word(0x2f00+offset,0x03e00008); irq_word(0x2f04+offset,0x24000000|ordinal);
        }
        irq_word(0x2c64,0x2f80);
        irq_word(0x2c44,0x2fc0); irq_word(0x2c60,0x2fc0);
        // Return the negative sum of both initial path bytes to expose routing.
        constexpr std::uint32_t pair_driver[]{0x90a80000,0x90c90000,0,0x01091021,0x00021023,0x03e00008,0};
        for (unsigned i=0;i<std::size(pair_driver);++i) irq_word(0x2fc0+i*4,pair_driver[i]);
        // Bounded synthetic link: return the first relative-path byte, no terminator.
        constexpr std::uint32_t readlink_driver[]{0x10e00006,0x24020000,0x90a80000,0,0xa0c80000,
            0x24020001,0,0x03e00008,0};
        for (unsigned i=0;i<std::size(readlink_driver);++i) irq_word(0x2f80+i*4,readlink_driver[i]);
        irq_word(0x2ac0,0x41e00000); irq_word(0x2ac8,0x104);
        std::copy(std::begin(library),std::end(library),module.memory.begin()+0x1acc);
        irq_word(0x2ad4,0x03e00008); irq_word(0x2ad8,0x24000008);
        irq_word(0x2adc,0x03e00008); irq_word(0x2ae0,0x24000012);
        irq_word(0x2ae4,0x03e00008); irq_word(0x2ae8,0x24000016);
        irq_word(0x2aec,0x03e00008); irq_word(0x2af0,0x24000013);
        irq_word(0x2e50,0x41e00000); irq_word(0x2e58,0x104);
        std::copy(std::begin(library),std::end(library),module.memory.begin()+0x1e5c);
        irq_word(0x2e64,0x03e00008); irq_word(0x2e68,0x24000014);
        irq_word(0x2e6c,0x03e00008); irq_word(0x2e70,0x24000024);
        irq_word(0x2d50,0x41e00000); irq_word(0x2d58,0x104);
        std::copy(std::begin(library),std::end(library),module.memory.begin()+0x1d5c);
        irq_word(0x2d64,0x03e00008); irq_word(0x2d68,0x24000011);
        irq_word(0x2d6c,0x03e00008); irq_word(0x2d70,0x24000029);
        irq_word(0x2ca0,0x41e00000); irq_word(0x2ca8,0x104);
        std::copy(std::begin(library),std::end(library),module.memory.begin()+0x1cac);
        irq_word(0x2cb4,0x03e00008); irq_word(0x2cb8,0x2400001b);
        irq_word(0x2cbc,0x03e00008); irq_word(0x2cc0,0x2400001d);
        for (const auto [address,ordinal] : {std::pair{0x2cc4U,21U},{0x2cccU,25U},{0x2cd4U,26U},{0x2cdcU,32U},{0x2ce4U,23U},{0x2cecU,30U}}) {
            irq_word(address,0x03e00008); irq_word(address+4,0x24000000|ordinal);
        }
        irq_word(0x2b90,0x24042dd0); irq_word(0x2b94,0x08000ab7); irq_word(0x2b98,0);
        constexpr std::uint32_t print_callback[]{0x8c880000,0,0xad050000,0x25080004,0xac880000,0x03e00008,0x2402ffff};
        for (unsigned i=0;i<std::size(print_callback);++i) irq_word(0x2dd0+i*4,print_callback[i]);
        irq_word(0x2c58,0x2c80);
        irq_word(0x2c5c,0x2e80);
        irq_word(0x2c50,0x2a80);
        // Mount writes the observed ABI arguments into its synthetic payload.
        constexpr std::uint32_t mount_driver[]{0x8fa80010,0x8fa90014,0x90aa0000,0x90cb0000,
            0x8c8c0004,0,0xad0a0000,0xad0b0004,0xad0c0008,0xad09000c,0x03e00008,0x00e01021};
        for (unsigned i=0;i<std::size(mount_driver);++i) irq_word(0x2a80+i*4,mount_driver[i]);
        // Read the shifted O32 arguments and expose path, unit and input length.
        constexpr std::uint32_t devctl_driver[]{0x8fa80010,0x8fa90014,0x8faa0018,0x90ab0000,
            0x8c8c0004,0,0xad2b0000,0xad2c0004,0xad280008,0xad2a000c,
            0x90eb0000,0,0xad2b0010,0x03e00008,0x00c01021};
        for (unsigned i=0;i<std::size(devctl_driver);++i) irq_word(0x2e80+i*4,devctl_driver[i]);
        for (const auto table : {0x28f0U,0x2c00U}) {
            irq_word(table+36,0x2e00); irq_word(table+40,0x2e40); irq_word(table+44,0x2880);
        }
        // Path operation exposes the unit and relative name through its result.
        constexpr std::uint32_t remove_driver[]{0x8c880004,0x90a90000,0,0x01091021,0x03e00008,0};
        constexpr std::uint32_t mkdir_driver[]{0x03e00008,0x00c01021};
        for (unsigned i=0;i<std::size(remove_driver);++i) irq_word(0x2e00+i*4,remove_driver[i]);
        for (unsigned i=0;i<std::size(mkdir_driver);++i) irq_word(0x2e40+i*4,mkdir_driver[i]);
        irq_word(0x2910,0x2d80); irq_word(0x2c20,0x2d80); irq_word(0x2c68,0x2da0);
        // ioctl accepts a scalar parameter and returns it unchanged.
        constexpr std::uint32_t control_driver[]{0xac85000c,0x03e00008,0x00c01021};
        for (unsigned i=0;i<std::size(control_driver);++i) irq_word(0x2d80+i*4,control_driver[i]);
        // ioctl2 copies one byte only when both extents are nonzero; returns cmd.
        constexpr std::uint32_t control2_driver[]{0x8fa80010,0x8fa90014,0x10e00007,0x00a01021,
            0x11200005,0,0x90ca0000,0,0xa10a0000,0xac87000c,0x03e00008,0};
        for (unsigned i=0;i<std::size(control2_driver);++i) irq_word(0x2da0+i*4,control2_driver[i]);
        irq_word(0x2c30,0x2b00); irq_word(0x2c34,0x2b80); irq_word(0x2c38,0x2d00);
        auto extended_read=std::to_array(directory_read);
        extended_read[6]=0x240921d0; // Extended regular-file type and access bits.
        extended_read[12]=0xaca90040; extended_read[14]=0xa0a90044; extended_read[15]=0xa0a00045;
        extended_read[16]=0xaca00140; // Extended private word at byte 320.
        for (unsigned i=0;i<extended_read.size();++i) irq_word(0x2d00+i*4,extended_read[i]);
        // Echo both offset words and record the stack-supplied origin.
        constexpr std::uint32_t seek64_driver[]{0x00c01021,0x00e01821,0x8fa80010,0,0xac88000c,0x03e00008,0};
        for (unsigned i=0;i<std::size(seek64_driver);++i) irq_word(0x2c80+i*4,seek64_driver[i]);
        irq_word(0x2340,0x41e00000); irq_word(0x2348,0x102);
        std::copy(std::begin(heap_name),std::end(heap_name),module.memory.begin()+0x134c);
        irq_word(0x2354,0x03e00008); irq_word(0x2358,0x2400000e);
        irq_word(0x2400,0x41e00000); irq_word(0x2408,0x101);
        constexpr char commands_name[]="sifcmd";
        std::copy(std::begin(commands_name),std::end(commands_name),module.memory.begin()+0x140c);
        const unsigned command_ordinals[]{8,9,10,11,12,13,28,29,4,6,7,14};
        for (unsigned index=0;index<std::size(command_ordinals);++index) {
            irq_word(0x2414+index*8,0x03e00008); irq_word(0x2418+index*8,0x24000000|command_ordinals[index]);
        }
        constexpr std::uint32_t command_registration[]{0x03e08021,0x24040001,0x240524c0,0x3406cf00,
            0x0c000909,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(command_registration);++index) irq_word(0x2480+index*4,command_registration[index]);
        constexpr std::uint32_t command_handler[]{0x90880010,0x8ca90000,0,0x01094021,0xaca80000,0x03e00008,0};
        for (unsigned index=0;index<std::size(command_handler);++index) irq_word(0x24c0+index*4,command_handler[index]);
        irq_word(0x2700,0x41e00000); irq_word(0x2708,0x101);
        constexpr char semaphore_name[]="thsemap";
        std::copy(std::begin(semaphore_name),std::end(semaphore_name),module.memory.begin()+0x170c);
        const unsigned semaphore_ordinals[]{4,5,6,7,8,9,11,12};
        for (unsigned index=0;index<std::size(semaphore_ordinals);++index) {
            irq_word(0x2714+index*8,0x03e00008); irq_word(0x2718+index*8,0x24000000|semaphore_ordinals[index]);
        }
        irq_word(0x2780,0x41e00000); irq_word(0x2788,0x101);
        constexpr char mailbox_name[]="thmsgbx";
        std::copy(std::begin(mailbox_name),std::end(mailbox_name),module.memory.begin()+0x178c);
        for (unsigned index=0;index<std::size(semaphore_ordinals);++index) {
            irq_word(0x2794+index*8,0x03e00008); irq_word(0x2798+index*8,0x24000000|semaphore_ordinals[index]);
        }
        irq_word(0x2500,0x41e00000); irq_word(0x2508,0x101);
        std::copy(std::begin(commands_name),std::end(commands_name),module.memory.begin()+0x150c);
        const unsigned rpc_ordinals[]{17,19,24,25,20,21,22,23};
        for (unsigned index=0;index<std::size(rpc_ordinals);++index) {
            irq_word(0x2514+index*8,0x03e00008); irq_word(0x2518+index*8,0x24000000|rpc_ordinals[index]);
        }
        constexpr std::uint32_t rpc_registration[]{0x03e08021,0x3404e000,0x24051234,0x240625c0,0x3407e100,
            0x341de400,0x0c000945,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(rpc_registration);++index) irq_word(0x2580+index*4,rpc_registration[index]);
        irq_word(0x25c0,0x24820005); irq_word(0x25c4,0x03e00008); irq_word(0x25c8,0);
        constexpr std::uint32_t rpc_worker[]{0x03e08021,0x00808821,0x0c000647,0,0x02202021,0x0c00094d,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(rpc_worker);++index) irq_word(0x2600+index*4,rpc_worker[index]);
        constexpr std::uint32_t rpc_function[]{0x8ca80000,0,0x01044021,0x01064021,0xaca80000,0x00a01021,0x03e00008,0};
        for (unsigned index=0;index<std::size(rpc_function);++index) irq_word(0x2680+index*4,rpc_function[index]);
        // This callback is private to another module, not an export or explicit root.
        provider.memory.resize(0xa0); provider.initialized_size=0x80;
        constexpr std::uint32_t remote_handler[]{0x8c880000,0,0x25080001,0xac880000,0x24020001,0x03e00008,0};
        for (unsigned index=0;index<std::size(remote_handler);++index)
            fixture::put32(provider.memory,0x60+index*4,remote_handler[index]);
        constexpr std::uint32_t remote_registration[]{0x03e08021,0x24040007,0x24050002,0x3406a060,0x3407c014,
            0x0c0005c5,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(remote_registration);++index) irq_word(0x1b40+index*4,remote_registration[index]);
        // A constant descriptor address points into another module's initialized data.
        fixture::put32(provider.memory,0x40,0x02200000);
        fixture::put32(provider.memory,0x48,0x80001e80);
        fixture::put32(provider.memory,0x4c,0x200); fixture::put32(provider.memory,0x50,1);
        constexpr std::uint32_t create_from_data[]{0x03e08021,0x3c04a000,0x3484a040,0x0c000651,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(create_from_data);++index) irq_word(0x1f20+index*4,create_from_data[index]);
        constexpr std::uint32_t stack_descriptor[]{0x03e08021,0x27bdffe0,0x27a40000,0xaca00000,0xaca00004,
            0x24082f60,0xac880008,0x24080200,0xac88000c,0x24080001,0xac880010,
            0x0c000651,0,0x27bd0020,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(stack_descriptor);++i) irq_word(0x1f60+i*4,stack_descriptor[i]);
        irq_word(0x2f60,0x03e00008); irq_word(0x2f64,0x24020049);
        constexpr std::uint32_t switch_code[]{0x2c820003,0x1040000a,0,0x00041080,0x3c010000,
            0x00220821,0x8c221fb0,0,0x00400008,0};
        for (unsigned i=0;i<std::size(switch_code);++i) irq_word(0x1fc0+i*4,switch_code[i]);
        irq_word(0x1ff0,0x03e00008); irq_word(0x1ff4,0x24020063);
        for (unsigned i=0;i<3;++i) {
            irq_word(0x1fb0+i*4,0x2f68+i*8);
            irq_word(0x2f68+i*8,0x03e00008); irq_word(0x2f6c+i*8,0x24020000|(11*(i+1)));
        }
        // 0x3000-0x45ff and the heap arena stay data for existing service tests.
        module.memory.resize(0x3900);
        irq_word(0x4800,0x41e00000); irq_word(0x4804,0); irq_word(0x4808,0x00000102);
        constexpr char dma_name[]="dmacman";
        std::copy(std::begin(dma_name),std::end(dma_name),module.memory.begin()+0x380c);
        constexpr unsigned dma_ordinals[]{16,33,34,17,28,32,8};
        for (unsigned index=0;index<std::size(dma_ordinals);++index) {
            irq_word(0x4814+index*8,0x03e00008); irq_word(0x4818+index*8,0x24000000|dma_ordinals[index]);
        }
        constexpr std::uint32_t dma_sequence[]{0x03e08021,0x3c041234,0x34845678,0x0c001205,0,0x24040009,0x0c001207,0x24050005,
            0x24040009,0x0c001209,0,0x0c00120b,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(dma_sequence);++index) irq_word(0x4860+index*4,dma_sequence[index]);
        irq_word(0x48a0,0x0c00120d); irq_word(0x48a4,0x24040003); irq_word(0x48a8,0x03e00008);
        irq_word(0x48b0,0x0c00120f); irq_word(0x48b4,0x24040009); irq_word(0x48b8,0x03e00008);
        irq_word(0x48c0,0x0c001207); irq_word(0x48c4,0x2404000d); irq_word(0x48c8,0x03e00008);
        constexpr std::uint32_t dma_start[]{0x24040003,0x3c050100,0x0c001211,0x34a50200,0x03e00008,0};
        for (unsigned index=0;index<std::size(dma_start);++index) irq_word(0x48d0+index*4,dma_start[index]);
        module.initialized_size=static_cast<std::uint32_t>(module.memory.size());
        for (const auto overwrite : {0xa0800008U,0xa0a00000U}) {
            auto invalidated=module;
            fixture::put32(invalidated.memory,0xf7c,overwrite);
            const std::array roots{0x1f60U};
            const std::array modules{invalidated,provider};
            const auto discovered=nt::iop_codegen::ir::discover_bundle(modules,roots);
            if (discovered.instructions.contains(0x2f60)) throw std::runtime_error("Aliased stack write retained a stale callback candidate");
        }
        std::vector<std::uint32_t> bundle_roots=entries;
        bundle_roots.insert(bundle_roots.end(),{0x13ec,0x1418,0x1420,0x1514,0x151c,0x1524,0x152c,0x1614,0x161c,0x1624,0x162c,0x171c,0x1724,0x172c,0x1734,0x173c,0x1744,0x174c,0x1754,0x17a0,0x1ac0,0x1ac8,0x1ad0,0x1ad8,0x1ae0,0x1a60,0x1a70,0x1a40,0x1a50,0x1b00,0x1b40,0x1c00,0x1d00,0x1d80,0x1dc0,0x1e00,0x1e40,0x1f20,0x1f60,0x1fc0,0x2140,0x2180,0x21c0,0x2314,0x2354,0x2394,0x239c,0x23a4,0x23ac,0x23b4,0x23bc,0x23c4,0x23cc,0x23d4,0x23dc,0x23e4,0x23ec,0x2f14,0x2f1c,0x2f24,0x2f2c,0x2f34,0x2f3c,0x2f44,0x2f4c,0x2ad4,0x2ae4,0x2aec,0x2cb4,0x2cbc,0x2cc4,0x2ccc,0x2cd4,0x2cdc,0x2ce4,0x2cec,0x2d64,0x2d6c,0x2e64,0x2e6c,0x2b90,0x2814,0x2824,0x282c,0x2840,0x2ba0,0x2414,0x241c,0x242c,0x2434,0x243c,0x2444,0x244c,0x2454,0x245c,0x2464,0x246c,0x2480,0x251c,0x2524,0x252c,0x2534,0x253c,0x2544,0x254c,0x2580,0x2600,0x2680,0x2714,0x271c,0x2724,0x272c,0x2734,0x273c,0x2744,0x274c,0x2794,0x279c,0x27a4,0x27ac,0x27b4,0x27bc,0x27c4,0x27cc});
        for (unsigned index=0;index<std::size(thread_ordinals);++index)
            if (thread_ordinals[index]!=4 && thread_ordinals[index]!=35) bundle_roots.push_back(0x1914+index*8);
        for (unsigned index=0;index<std::size(event_ordinals);++index) bundle_roots.push_back(0x1814+index*8);
        for (unsigned index=0;index<std::size(sif_ordinals);++index) bundle_roots.push_back(0x2214+index*8);
        nt::elf::IopModule dispatch{0xa100,0xa100,nt::Bytes(0x600),0x600,{}};
        const auto dispatch_word=[&](unsigned pc,std::uint32_t word) { fixture::put32(dispatch.memory,pc-dispatch.base,word); };
        dispatch_word(0xa100,0x03e00008);
        for (unsigned variant=0;variant<4;++variant) {
            const auto base=0xa140+variant*0x80;
            const bool delay_shift=variant&1,call=variant&2;
            dispatch_word(base,0x03e08021); // Preserve caller for the call-table variants.
            dispatch_word(base+4,0x2c820003);
            dispatch_word(base+8,0x1040000b);
            const auto shift=base+(delay_shift ? 12 : 16);
            dispatch_word(shift,0x00041080);
            dispatch_word(shift+8,0x00220821); dispatch_word(shift+12,0x8c22a580);
            // Signed displacement requires the high half to include its carry.
            dispatch_word(shift+4,0x3c010001);
            dispatch_word(shift+20,call ? 0x0040f809 : 0x00400008);
            dispatch_word(shift+24,0x24070011);
            if (call) {
                dispatch_word(shift+28,0x0200f821);
                dispatch_word(shift+32,0x03e00008);
            }
            dispatch_word(base+0x38,0x0200f821);
            dispatch_word(base+0x3c,0x03e00008); dispatch_word(base+0x40,0x24020063);
            bundle_roots.push_back(base);
        }
        for (unsigned index=0;index<3;++index) {
            dispatch_word(0xa580+index*4,0xa500+index*8);
            dispatch_word(0xa500+index*8,0x03e00008); dispatch_word(0xa504+index*8,0x24020000|(11*(index+1)));
        }
        for (unsigned index=0;index<4;++index) {
            constexpr unsigned operations[]{34,38,42,46};
            const auto base=0xa380+index*16;
            dispatch_word(base,(operations[index]<<26)|(4U<<21)|(2U<<16));
            dispatch_word(base+4,0x03e00008); bundle_roots.push_back(base);
        }
        // Paired partial loads across adjacent words, including reversed order.
        for (unsigned reverse=0;reverse<2;++reverse) {
            const auto base=0xa3c0+reverse*0x20;
            dispatch_word(base,reverse ? 0x98820000 : 0x88820003);
            dispatch_word(base+4,reverse ? 0x88820003 : 0x98820000);
            dispatch_word(base+8,0x00401821); // Still sees the old architectural value.
            dispatch_word(base+12,0x03e00008); bundle_roots.push_back(base);
        }
        nt::elf::IopModule factories{0xb000,0xb000,nt::Bytes(0x600),0x600,{}};
        const auto factory_word=[&](unsigned pc,std::uint32_t word) { fixture::put32(factories.memory,pc-factories.base,word); };
        factory_word(0xb4c0,0x41e00000); factory_word(0xb4c8,0x201);
        constexpr char archive_library[]="romdrv";
        std::copy(std::begin(archive_library),std::end(archive_library),factories.memory.begin()+0x4cc);
        factory_word(0xb4d4,0x03e00008); factory_word(0xb4d8,0x24000004);
        factory_word(0xb4dc,0x03e00008); factory_word(0xb4e0,0x24000005);
        bundle_roots.insert(bundle_roots.end(),{0xb4d4,0xb4dc});
        constexpr std::uint32_t factory_caller[]{0x03e08021,0x0c002c10,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(factory_caller);++i) factory_word(0xb000+i*4,factory_caller[i]);
        constexpr std::uint32_t factory_selector[]{0x2c820003,0x1040000a,0,0x00041080,0x3c010001,
            0x00220821,0x8c22b300,0,0x00400008,0};
        for (unsigned i=0;i<std::size(factory_selector);++i) factory_word(0xb040+i*4,factory_selector[i]);
        factory_word(0xb070,0x03e00008); factory_word(0xb074,0x00001021);
        for (unsigned i=0;i<3;++i) {
            factory_word(0xb300+i*4,0xb080+i*16);
            factory_word(0xb080+i*16,0x03e00008);
            factory_word(0xb084+i*16,0x3402b100+i*8); // Pointer defined in return delay slot.
            factory_word(0xb100+i*8,0x03e00008); factory_word(0xb104+i*8,0x24020050+i);
        }
        // Table address is hoisted in s2, outside the dispatch pattern.
        constexpr std::uint32_t hoisted[]{0x3c120001,0x2652b300,0,0,0x2c820003,0x10400008,
            0x00041080,0x00521021,0x8c420000,0,0x00400008,0,0,0,0x03e00008,0x00001021};
        for (unsigned i=0;i<std::size(hoisted);++i) factory_word(0xb180+i*4,hoisted[i]);
        bundle_roots.push_back(0xb180);
        constexpr std::uint32_t hoisted_after_delay[]{0x3c120001,0x2652b300,0x2c820003,0x1040000a,
            0x00008821,0x00041080,0x00521021,0x8c420000,0,0x00400008,0,0,0,0,0x03e00008,0x00001021};
        constexpr std::uint32_t spilled_predicate[]{0x2c820003,0xafbf0000,0x1040000b,0,0x00041080,
            0x3c010001,0x00220821,0x8c22b300,0,0x00400008,0,0,0,0,0x03e00008,0x00001021};
        for (unsigned i=0;i<std::size(hoisted_after_delay);++i) factory_word(0xb320+i*4,hoisted_after_delay[i]);
        for (unsigned i=0;i<std::size(spilled_predicate);++i) factory_word(0xb380+i*4,spilled_predicate[i]);
        bundle_roots.insert(bundle_roots.end(),{0xb320,0xb380});
        factory_word(0xb400,0x41e00000); factory_word(0xb408,0x107);
        constexpr char modload_name[]="modload";
        std::copy(std::begin(modload_name),std::end(modload_name),factories.memory.begin()+0x40c);
        constexpr unsigned loader_ordinals[]{12,13,14,7,16,17,18,22,23};
        for (unsigned i=0;i<std::size(loader_ordinals);++i) {
            factory_word(0xb414+i*8,0x03e00008); factory_word(0xb418+i*8,0x24000000|loader_ordinals[i]);
            bundle_roots.push_back(0xb414+i*8);
        }
        factory_word(0xb480,0x41e00000); factory_word(0xb488,0x103);
        constexpr char cache_library[]="loadcore";
        std::copy_n(cache_library,8,factories.memory.begin()+0x48c);
        for (unsigned i=0;i<2;++i) {
            factory_word(0xb494+i*8,0x03e00008); factory_word(0xb498+i*8,0x24000004+i);
            bundle_roots.push_back(0xb494+i*8);
        }
        constexpr std::uint32_t register_loader[]{0x03e08021,0x3404b280,0x3405b288,0x3406b290,
            0x0c002d05,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(register_loader);++i) factory_word(0xb200+i*4,register_loader[i]);
        constexpr std::uint32_t register_path[]{0x03e08021,0x3404b298,0x0c002d07,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(register_path);++i) factory_word(0xb240+i*4,register_path[i]);
        for (unsigned i=0;i<4;++i) {
            factory_word(0xb280+i*8,0x03e00008); factory_word(0xb284+i*8,0x24020060+i);
        }
        bundle_roots.insert(bundle_roots.end(),{0xb200,0xb240});
        constexpr std::uint32_t stack_callback[]{0x03e08021,0x27bdfff0,0x3408b580,0xafa80000,
            0x0c002d25,0,0x8fa80000,0,0x11000003,0,0x0100f809,0,
            0x27bd0010,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(stack_callback);++i) factory_word(0xb500+i*4,stack_callback[i]);
        factory_word(0xb580,0x03e00008); factory_word(0xb584,0x24020073);
        bundle_roots.push_back(0xb500);
        constexpr std::uint32_t delay_callback[]{0x03e08021,0x3409b588,0x0c002d25,0xafa90004,
            0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(delay_callback);++i) factory_word(0xb540+i*4,delay_callback[i]);
        factory_word(0xb588,0x03e00008); factory_word(0xb58c,0x24020074);
        bundle_roots.push_back(0xb540);
        {
            auto unknown=factories; fixture::put32(unknown.memory,0x84,0x8c820000);
            const auto analyzed=nt::iop_codegen::ir::discover(unknown);
            if (analyzed.instructions.contains(0xb100))
                throw std::runtime_error("Unknown return value was treated as a callback address");
        }
        // Constant-address loads. The jump's delay slot writes the pointer register
        // and must not fall through into the jalr.
        nt::elf::IopModule pointers{0x10000,0x10000,nt::Bytes(0x240),0x240,{}};
        const auto pointer_word=[&](unsigned pc,std::uint32_t word) { fixture::put32(pointers.memory,pc-pointers.base,word); };
        pointer_word(0x10000,0x03e00008); pointer_word(0x10004,0x24020021);
        pointer_word(0x10008,0x03e00008); pointer_word(0x1000c,0x24020022);
        constexpr std::uint32_t pointer_a[]{0x03e08021,0x3c020001,0x8c420180,0,0x14400003,0,0x08004016,0x24020001,
            0x0040f809,0,0x0200f821,0x03e00008,0,0,0x03e00008,0};
        for (unsigned index=0;index<std::size(pointer_a);++index) pointer_word(0x10020+index*4,pointer_a[index]);
        constexpr std::uint32_t pointer_b[]{0x03e08021,0x3c030001,0x8c630184,0,0x14600003,0,0x08004028,0x24020001,
            0x0060f809,0,0x0200f821,0x03e00008,0,0,0,0,0x03e00008,0};
        for (unsigned index=0;index<std::size(pointer_b);++index) pointer_word(0x10060+index*4,pointer_b[index]);
        constexpr std::uint32_t pointer_store[]{0x3c010001,0x3c030001,0x24630008,0xac230184,0x03e00008,0};
        for (unsigned index=0;index<std::size(pointer_store);++index) pointer_word(0x100b0+index*4,pointer_store[index]);
        // An unknown word store keeps one fixed slot open. The leaf is a root, not a proved callee.
        constexpr std::uint32_t open_slot[]{0x03e08021,0x3c010001,0xac240190,0x8c220190,0,0x0040f809,0x24030001,
            0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(open_slot);++index) pointer_word(0x100d0+index*4,open_slot[index]);
        pointer_word(0x10100,0x03e00008); pointer_word(0x10104,0x24020033);
        pointer_word(0x10180,0x00010000);
        pointer_word(0x10190,0x00010100);
        // Word stores in the published SIO2 block, then the same word through its cached alias.
        constexpr std::uint32_t sio2_words[]{0x3c03bf80,0x34638200,0xac640000,0xac650080,0xac660068,
            0x3c029f80,0x34428268,0x8c420000,0,0x03e00008,0};
        for (unsigned index=0;index<std::size(sio2_words);++index) pointer_word(0x101a0+index*4,sio2_words[index]);
        constexpr std::uint32_t sio2_read[]{0x3c03bf80,0x34638200,0x8c620000,0,0x8c630080,0,0x03e00008,0};
        for (unsigned index=0;index<std::size(sio2_read);++index) pointer_word(0x101cc+index*4,sio2_read[index]);
        constexpr std::uint32_t sio2_miss[]{0x3c02bf80,0x34428400,0xac440000,0x03e00008,0};
        for (unsigned index=0;index<std::size(sio2_miss);++index) pointer_word(0x101ec+index*4,sio2_miss[index]);
        constexpr std::uint32_t sio2_byte[]{0x3c02bf80,0x34428268,0xa0440000,0x03e00008,0};
        for (unsigned index=0;index<std::size(sio2_byte);++index) pointer_word(0x10200+index*4,sio2_byte[index]);
        constexpr std::uint32_t serial_output[]{0x3c02bf80,0x34428260,0xa0440000,0x03e00008,0};
        for (unsigned index=0;index<std::size(serial_output);++index) pointer_word(0x10220+index*4,serial_output[index]);
        bundle_roots.push_back(0x10220);
        bundle_roots.insert(bundle_roots.end(),{0x4860U,0x48a0U,0x48b0U,0x48c0U,0x48d0U,0x10020U,0x10060U,0x100b0U,0x100d0U,0x10100U,
            0x101a0U,0x101ccU,0x101ecU,0x10200U});
        // A callback address escapes only through a compiled-library argument.
        // The callback itself is deliberately absent from explicit roots.
        factory_word(0xb5c0,0x41e00000); factory_word(0xb5c8,0x100);
        std::copy(std::begin(worker_name),std::end(worker_name),factories.memory.begin()+0x5cc);
        factory_word(0xb5d4,0x03e00008); factory_word(0xb5d8,0x24000000);
        constexpr std::uint32_t escaped[]{0x03e08021,0x0c002d75,0x3404b5e8,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(escaped);++i) factory_word(0xb5a0+i*4,escaped[i]);
        factory_word(0xb5e8,0x03e00008); factory_word(0xb5ec,0x24020075);
        bundle_roots.push_back(0xb5a0);
        nt::elf::IopModule display{0x11000,0x11014,nt::Bytes(0x300),0x300,{}};
        const auto display_word=[&](unsigned pc,std::uint32_t word) { fixture::put32(display.memory,pc-display.base,word); };
        display_word(0x11000,0x41e00000); display_word(0x11008,0x101);
        constexpr char display_library[]="vblank";
        std::copy(std::begin(display_library),std::end(display_library),display.memory.begin()+12);
        for (unsigned ordinal=4;ordinal<=9;++ordinal) {
            const auto pc=0x11014+(ordinal-4)*8;
            display_word(pc,0x03e00008); display_word(pc+4,0x24000000|ordinal);
            bundle_roots.push_back(pc);
        }
        // Runtime registry pointer returned by a compiled helper, copied to v1
        // before invocation. The pointed-to code is a relocation candidate.
        constexpr std::uint32_t registry_caller[]{0x03e08021,0x0c004458,0,0x00401821,
            0x10600003,0,0x0060f809,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(registry_caller);++index) display_word(0x11100+index*4,registry_caller[index]);
        constexpr std::uint32_t nested_registry[]{0x03e08021,0x00851821,0x8c630000,0,0x8c62000c,0,
            0x0040f809,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(nested_registry);++index) display_word(0x11130+index*4,nested_registry[index]);
        constexpr std::uint32_t registry_helper[]{0x00851021,0x8c420000,0,0x03e00008,0};
        for (unsigned index=0;index<std::size(registry_helper);++index) display_word(0x11160+index*4,registry_helper[index]);
        display_word(0x111a0,0x03e00008); display_word(0x111a4,0x24020049);
        display_word(0x111c0,0x41c00000); display_word(0x111c8,0x101);
        constexpr char registry_library[]="regtest";
        std::copy(std::begin(registry_library),std::end(registry_library),display.memory.begin()+0x1cc);
        display_word(0x111d4,0x111a0);
        display.code_addresses={0x111a0};
        bundle_roots.push_back(0x11100);
        bundle_roots.push_back(0x11130);
        constexpr std::uint32_t field_registry[]{0x03e08021,0x8c820038,0,0x8c420008,0,
            0x0040f809,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(field_registry);++i) display_word(0x11200+i*4,field_registry[i]);
        bundle_roots.push_back(0x11200);
        {
            auto clobbered=display; fixture::put32(clobbered.memory,0x210,0x00a01021);
            const std::array roots{0x11200U};
            bool rejected=false;
            try { nt::iop_codegen::ir::discover(clobbered,roots); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Unknown overwrite retained a nested descriptor callback");
        }
        constexpr std::uint32_t assigned_callback[]{0x03e08021,0x3c030001,0xac6512b0,0x8c6212b0,0,
            0x0040f809,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(assigned_callback);++i) display_word(0x11280+i*4,assigned_callback[i]);
        bundle_roots.push_back(0x11280);
        nt::elf::IopModule registry_client{0x12000,0x12060,nt::Bytes(0x100),0x100,{}};
        fixture::put32(registry_client.memory,0,0x41e00000);
        fixture::put32(registry_client.memory,8,0x101);
        std::copy(std::begin(registry_library),std::end(registry_library),registry_client.memory.begin()+12);
        fixture::put32(registry_client.memory,0x14,0x03e00008); fixture::put32(registry_client.memory,0x18,0x24000000);
        fixture::put32(registry_client.memory,0x40,0x03e00008); fixture::put32(registry_client.memory,0x44,0x24020057);
        constexpr std::uint32_t registry_register[]{0x03e08021,0x3c040001,0x34842080,0x0c004805,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(registry_register);++i) fixture::put32(registry_client.memory,0x60+i*4,registry_register[i]);
        fixture::put32(registry_client.memory,0x80,0x03e00008); fixture::put32(registry_client.memory,0x84,0x24020063);
        registry_client.code_addresses={0x12040};
        {
            const std::array roots{0x11100U};
            const auto discovered=nt::iop_codegen::ir::discover(display,roots);
            if (!discovered.guarded_callback_sites.contains(0x11118) || !discovered.instructions.contains(0x111a0))
                throw std::runtime_error("Returned indexed callback was not discovered and guarded");
            auto overwritten=display; fixture::put32(overwritten.memory,0x10c,0x00c01821);
            bool rejected=false;
            try { nt::iop_codegen::ir::discover(overwritten,roots); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Unknown overwrite retained a returned callback relationship");
        }
        nt::elf::IopModule sound{0x13000,0x13100,nt::Bytes(0x300),0x300,{}};
        fixture::put32(sound.memory,0,0x41c00000); fixture::put32(sound.memory,8,0x104);
        constexpr char sound_name[]="libsd";
        std::copy(std::begin(sound_name),std::end(sound_name),sound.memory.begin()+12);
        for (unsigned ordinal=0;ordinal<28;++ordinal) {
            const auto offset=0x100+(ordinal==3 ? 1U : ordinal)*8;
            fixture::put32(sound.memory,20+ordinal*4,sound.base+offset);
            fixture::put32(sound.memory,offset,0xfc000000); // Must never decode a replaced public entry.
        }
        {
            const auto replaced=nt::iop_codegen::ir::discover(sound);
            if (replaced.instructions.size()!=27 || replaced.instructions.at(0x13100).immediate!=0x150000)
                throw std::runtime_error("Native sound public entries were not lowered to service calls");
            auto invalid=sound; fixture::put32(invalid.memory,8,0x204);
            bool rejected=false;
            try { nt::iop_codegen::ir::discover(invalid); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Unknown sound ABI accepted a native replacement");
            rejected=false;
            const std::array private_entry{0x132f0U};
            try { nt::iop_codegen::ir::discover(sound,private_entry); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Native sound replacement exposed private console implementation code");
        }
        nt::elf::IopModule usb{0x14000,0x14200,nt::Bytes(0x300),0x300,{}};
        fixture::put32(usb.memory,0,0x41c00000); fixture::put32(usb.memory,8,0x102);
        constexpr char usb_name[]="usbd";
        std::copy(std::begin(usb_name),std::end(usb_name),usb.memory.begin()+12);
        for (unsigned ordinal=0;ordinal<19;++ordinal) {
            const auto offset=0x100+(ordinal==0 || ordinal==3 ? 1U : ordinal)*8;
            fixture::put32(usb.memory,20+ordinal*4,usb.base+offset);
            fixture::put32(usb.memory,offset,0xfc000000);
        }
        fixture::put32(usb.memory,0x200,0xfc000000);
        nt::elf::IopModule usb_client{0x15000,0x15000,nt::Bytes(0x600),0x600,nt::elf::IopModuleInfo{"usb-client",0x100,0,0x300,0x300,0}};
        const auto usb_put=[&](unsigned offset,unsigned value) { fixture::put32(usb_client.memory,offset,value); };
        constexpr std::uint32_t usb_start[]{0x27bdfff0,0xafbf000c,0x3c040001,0x34845440,
            0x0c0054bd,0,0x8fbf000c,0x27bd0010,0x03e00008,0};
        for (unsigned i=0;i<std::size(usb_start);++i) usb_put(i*4,usb_start[i]);
        // Probe records its module GP, counts calls and claims each test device.
        constexpr std::uint32_t usb_probe[]{0x3c080001,0x35085400,0x8d090000,0xad1c0008,
            0x25290001,0xad090000,0x03e00008,0x24020001};
        constexpr std::uint32_t usb_connect[]{0x3c080001,0x35085404,0x8d090000,0,
            0x25290001,0xad090000,0x03e00008,0x24020000};
        for (unsigned i=0;i<std::size(usb_probe);++i) usb_put(0x80+i*4,usb_probe[i]);
        for (unsigned i=0;i<std::size(usb_connect);++i) usb_put(0xa0+i*4,usb_connect[i]);
        usb_put(0xc0,0x03e00008); usb_put(0xc4,0x24020000);
        constexpr std::uint32_t usb_completion[]{0xacc40000,0xacc50004,0xacdc0008,0x8cc8000c,0,
            0x25080001,0xacc8000c,0x03e00008,0};
        for (unsigned i=0;i<std::size(usb_completion);++i) usb_put(0x100+i*4,usb_completion[i]);
        constexpr std::uint32_t usb_submit[]{0x27bdffe0,0xafbf001c,0x3c080001,0x35085100,0xafa80010,
            0x3c080001,0x35085700,0xafa80014,0x0c0054bf,0,0x8fbf001c,0x27bd0020,0x03e00008,0};
        for (unsigned i=0;i<std::size(usb_submit);++i) usb_put(0x140+i*4,usb_submit[i]);
        bundle_roots.push_back(0x15140);
        usb_put(0x448,0x15480); usb_put(0x44c,0x15080); usb_put(0x450,0x150a0); usb_put(0x454,0x150c0);
        usb_client.memory[0x480]='t';
        usb_put(0x2e0,0x41e00000); usb_put(0x2e8,0x102);
        std::copy(std::begin(usb_name),std::end(usb_name),usb_client.memory.begin()+0x2ec);
        usb_put(0x2f4,0x03e00008); usb_put(0x2f8,0x24000004);
        usb_put(0x2fc,0x03e00008); usb_put(0x300,0x2400000b);
        {
            const std::array modules{usb,usb_client};
            const auto discovered=nt::iop_codegen::ir::discover_bundle(modules);
            if (discovered.instructions.at(0x14200).immediate!=0x160100 ||
                !discovered.instructions.contains(0x15080) || !discovered.instructions.contains(0x150a0) ||
                !discovered.instructions.contains(0x150c0))
                throw std::runtime_error("USB public ABI and driver callbacks were not compiled");
        }
        nt::elf::IopModule hoisted_table{0x16000,0x16000,nt::Bytes(0x400),0x400,
            nt::elf::IopModuleInfo{"hoisted-table",0x100,0,0x200,0x200,0}};
        const auto table_put=[&](unsigned offset,unsigned value) { fixture::put32(hoisted_table.memory,offset,value); };
        constexpr std::uint32_t table_code[]{0x3c030001,0x24636300,0x00041080,0x00438021,
            0x00004021,0,0x2c820002,0x1040001c,0,0x8e020000,0,0x00400008,0};
        for (unsigned i=0;i<std::size(table_code);++i) table_put(i*4,table_code[i]);
        table_put(0x80,0x03e00008); table_put(0x84,0x2402000a);
        table_put(0x90,0x03e00008); table_put(0x94,0x2402000b);
        table_put(0x300,0x16080); table_put(0x304,0x16090);
        {
            auto broken=hoisted_table; fixture::put32(broken.memory,0x10,0x00a08021);
            bool rejected=false;
            try { nt::iop_codegen::ir::discover(broken); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Hoisted indexed table accepted a clobbered address register");
        }
        nt::elf::IopModule network_services{0x17000,0x17014,nt::Bytes(0xc00),0xc00,{}};
        fixture::put32(network_services.memory,0,0x41e00000); fixture::put32(network_services.memory,8,0x104);
        std::copy(std::begin(library),std::end(library),network_services.memory.begin()+12);
        constexpr unsigned network_ordinals[]{16,40,24,31,33,34,10,11,15,4,5};
        for (unsigned i=0;i<std::size(network_ordinals);++i) {
            fixture::put32(network_services.memory,20+i*8,0x03e00008);
            fixture::put32(network_services.memory,24+i*8,0x24000000|network_ordinals[i]);
            bundle_roots.push_back(0x17014+i*8);
        }
        fixture::put32(network_services.memory,0x100,0x41e00000); fixture::put32(network_services.memory,0x108,0x101);
        const std::string disc_library="cdvdman";
        std::copy(disc_library.begin(),disc_library.end(),network_services.memory.begin()+0x10c);
        fixture::put32(network_services.memory,0x114,0x03e00008); fixture::put32(network_services.memory,0x118,0x24000016);
        bundle_roots.push_back(0x17114);
        const auto network_word=[&](unsigned pc,unsigned word) { fixture::put32(network_services.memory,pc-0x17000,word); };
        network_word(0x17200,0x41e00000); network_word(0x17208,0x102);
        const std::string scheduling_library="thbase";
        std::copy(scheduling_library.begin(),scheduling_library.end(),network_services.memory.begin()+0x20c);
        for (unsigned ordinal=15;ordinal<=17;++ordinal) {
            const auto pc=0x17214+(ordinal-15)*8;
            network_word(pc,0x03e00008); network_word(pc+4,0x24000000|ordinal); bundle_roots.push_back(pc);
        }
        constexpr std::uint32_t jumping[]{0x03e0b821,0x2410002a,0x3c040001,0x34848000,
            0x0c005c17,0,0x14400007,0,0x24100063,0x24050000,0x3c040001,0x34848000,
            0x0c005c19,0,0x02001821,0x02e0f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(jumping);++i) network_word(0x17400+i*4,jumping[i]);
        bundle_roots.push_back(0x17400);
        constexpr std::uint32_t rotating[]{0x03e08021,0x24040000,0x0c005c87,0,0x0200f821,0x03e00008,0x2402002a};
        for (unsigned i=0;i<std::size(rotating);++i) {
            network_word(0x17500+i*4,rotating[i]);
            network_word(0x17580+i*4,i==2 ? 0x0c005c89 : i==6 ? 0x24020001 : rotating[i]);
        }
        bundle_roots.push_back(0x17500); bundle_roots.push_back(0x17580);
        constexpr std::uint32_t cursor_code[]{0x3c140001,0x26947600,0x00009021,0x00001821,
            0x2e420002,0x10400014,0x00008021,0x8e820000,0,0x00400008,0};
        for (unsigned i=0;i<std::size(cursor_code);++i) network_word(0x17300+i*4,cursor_code[i]);
        network_word(0x17330,0x2410000a); network_word(0x17334,0x08005cd2);
        network_word(0x1733c,0x24100014); network_word(0x17340,0x08005cd2);
        network_word(0x17348,0x00701821); network_word(0x1734c,0x26520001);
        network_word(0x17350,0x2a420003); network_word(0x17354,0x1440ffee); network_word(0x17358,0x26940004);
        network_word(0x17368,0x03e00008); network_word(0x1736c,0x00601021);
        network_word(0x17600,0x17330); network_word(0x17604,0x1733c);
        bundle_roots.push_back(0x17300);
        network_word(0x17800,0x41e00000); network_word(0x17808,0x101);
        std::copy(disc_library.begin(),disc_library.end(),network_services.memory.begin()+0x80c);
        constexpr unsigned cdvd_ordinals[]{6,8,11,12,13,14,28,37,39,83};
        for (unsigned i=0;i<std::size(cdvd_ordinals);++i) {
            network_word(0x17814+i*8,0x03e00008); network_word(0x17818+i*8,0x24000000|cdvd_ordinals[i]);
            bundle_roots.push_back(0x17814+i*8);
        }
        constexpr std::uint32_t cdvd_callback[]{0x3c080001,0x35088200,0xad040000,0x03e00008,0};
        constexpr std::uint32_t cdvd_register[]{0x03e08021,0x3c040001,0x34847900,0x0c005e13,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(cdvd_callback);++i) network_word(0x17900+i*4,cdvd_callback[i]);
        for (unsigned i=0;i<std::size(cdvd_register);++i) network_word(0x17920+i*4,cdvd_register[i]);
        bundle_roots.push_back(0x17920); // Callback must be discovered through registration.
        network_word(0x17a00,0x27bdffc0); network_word(0x17a04,0x2c820002);
        for (unsigned i=0;i<10;++i) network_word(0x17a08+i*4,0xafbf0000+i*4);
        constexpr std::uint32_t spilled_switch[]{0x10400010,0xafb00028,0x00041080,0x3c010001,
            0x00220821,0x8c227b00,0,0x00400008,0,0x24020007,0x03e00008,0x27bd0040,
            0x24020009,0x03e00008,0x27bd0040,0,0,0x2402ffff,0x03e00008,0x27bd0040};
        for (unsigned i=0;i<std::size(spilled_switch);++i) network_word(0x17a30+i*4,spilled_switch[i]);
        network_word(0x17b00,0x17a54); network_word(0x17b04,0x17a60);
        bundle_roots.push_back(0x17a00);
        // Original unrolled arithmetic tail, selected as end - index * 4.
        // An out-of-range index must not enter even another compiled word.
        constexpr std::uint32_t inline_tail[]{0x3c080001,0x35087734,0x00044880,0x01094022,
            0x24020001,0x01000008,0x24420002,0x24420004,0x24420008,0x24420010,
            0x24420020,0x24420040,0x24420080,0x03e00008,0x24420100};
        for (unsigned i=0;i<std::size(inline_tail);++i) network_word(0x17700+i*4,inline_tail[i]);
        bundle_roots.push_back(0x17700);
        {
            const std::array roots{0x17700U};
            const auto discovered=nt::iop_codegen::ir::discover(network_services,roots);
            const auto& targets=discovered.instructions.at(0x17714).targets;
            if (targets.size()!=7 || targets.front()!=0x1771c || targets.back()!=0x17734 ||
                !discovered.guarded_callback_sites.contains(0x17714))
                throw std::runtime_error("Inline tail entries were not discovered and guarded");
            for (const auto [offset,word] : {std::pair{0x708U,0x00044840U}, // wrong stride
                    std::pair{0x710U,0x00c74021U}, // clobbered target
                    std::pair{0x724U,0x10000000U}}) { // non-linear tail
                auto invalid=network_services; fixture::put32(invalid.memory,offset,word);
                bool rejected=false;
                try { nt::iop_codegen::ir::discover(invalid,roots); }
                catch (const std::runtime_error&) { rejected=true; }
                if (!rejected) throw std::runtime_error("Unproved inline tail shape was accepted");
            }
        }
        constexpr std::uint32_t nullable_registry[]{0x03e08021,0x8c820038,0,0x8c420008,0,
            0x14400003,0,0x0800449b,0x24020000,0x0040f809,0,0x0200f821,0x03e00008,0};
        for (unsigned i=0;i<std::size(nullable_registry);++i) display_word(0x11240+i*4,nullable_registry[i]);
        bundle_roots.push_back(0x11240);
        nt::elf::IopModule dev9{0x24000,0x24100,nt::Bytes(0x200),0x200,{}};
        fixture::put32(dev9.memory,0,0x41c00000); fixture::put32(dev9.memory,8,0x109);
        constexpr char dev9_name[]="dev9";
        std::copy(std::begin(dev9_name),std::end(dev9_name),dev9.memory.begin()+12);
        for (unsigned ordinal=0;ordinal<14;++ordinal) {
            const auto offset=0x100+(ordinal==3 ? 1U : ordinal)*8;
            fixture::put32(dev9.memory,20+ordinal*4,dev9.base+offset);
            fixture::put32(dev9.memory,offset,0xfc000000);
        }
        {
            const auto replaced=nt::iop_codegen::ir::discover(dev9);
            if (replaced.instructions.at(dev9.entry).immediate!=0x170100 || replaced.instructions.size()!=13)
                throw std::runtime_error("DEV9 public module entry was not lowered to native capability detection");
            auto invalid=dev9; fixture::put32(invalid.memory,8,0x209);
            bool rejected=false;
            try { nt::iop_codegen::ir::discover(invalid); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Unknown DEV9 ABI accepted a native replacement");
            rejected=false;
            const std::array private_entry{0x241f0U};
            try { nt::iop_codegen::ir::discover(dev9,private_entry); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("DEV9 replacement exposed private driver code");
        }
        nt::elf::IopModule fixed_pool{0x25000,0x25014,nt::Bytes(0x100),0x100,{}};
        fixture::put32(fixed_pool.memory,0,0x41e00000); fixture::put32(fixed_pool.memory,8,0x101);
        constexpr char pool_library[]="thfpool";
        std::copy(std::begin(pool_library),std::end(pool_library),fixed_pool.memory.begin()+12);
        const unsigned pool_ordinals[]{4,5,6,7,8,9,11,12};
        for (unsigned i=0;i<std::size(pool_ordinals);++i) {
            fixture::put32(fixed_pool.memory,20+i*8,0x03e00008);
            fixture::put32(fixed_pool.memory,24+i*8,0x24000000|pool_ordinals[i]);
            bundle_roots.push_back(fixed_pool.base+20+i*8);
        }
        nt::elf::IopModule edge_table{0x26000,0x26000,nt::Bytes(0x120),0x120,{}};
        const auto edge_word=[&](unsigned address,std::uint32_t word) { fixture::put32(edge_table.memory,address-edge_table.base,word); };
        // The range predicate is defined on an incoming branch edge, away from
        // the table dispatch block. All constants and instructions are synthetic.
        edge_word(0x26000,0x10a00007); edge_word(0x26004,0x2c880003);
        edge_word(0x26008,0x03e00008); edge_word(0x2600c,0x24020063);
        constexpr std::uint32_t edge_dispatch[]{0x11000017,0x00041080,0x3c010002,0x00220821,0x8c226100,0,0x00400008,0};
        for (unsigned i=0;i<std::size(edge_dispatch);++i) edge_word(0x26020+i*4,edge_dispatch[i]);
        for (unsigned i=0;i<3;++i) {
            edge_word(0x26060+i*8,0x03e00008); edge_word(0x26064+i*8,0x24020011+i*2);
            edge_word(0x26100+i*4,0x26060+i*8);
        }
        edge_word(0x26080,0x03e00008); edge_word(0x26084,0x2402005b);
        bundle_roots.push_back(edge_table.entry);
        nt::elf::IopModule rpc_client{0x27000,0x27014,nt::Bytes(0x120),0x120,{}};
        fixture::put32(rpc_client.memory,0,0x41e00000); fixture::put32(rpc_client.memory,8,0x101);
        constexpr char rpc_client_library[]="sifcmd";
        std::copy(std::begin(rpc_client_library),std::end(rpc_client_library),rpc_client.memory.begin()+12);
        const unsigned rpc_client_ordinals[]{15,16,18};
        for (unsigned i=0;i<std::size(rpc_client_ordinals);++i) {
            fixture::put32(rpc_client.memory,20+i*8,0x03e00008);
            fixture::put32(rpc_client.memory,24+i*8,0x24000000|rpc_client_ordinals[i]);
            bundle_roots.push_back(rpc_client.base+20+i*8);
        }
        constexpr std::uint32_t rpc_client_callback[]{0xac9c0000,0x2402004d,0xac820004,0x03e00008,0};
        for (unsigned i=0;i<std::size(rpc_client_callback);++i) fixture::put32(rpc_client.memory,0x100+i*4,rpc_client_callback[i]);
        bundle_roots.push_back(0x27100);
        const std::array bundle{module,provider,dispatch,factories,pointers,display,registry_client,sound,usb,usb_client,hoisted_table,network_services,dev9,fixed_pool,edge_table,rpc_client};
        {
            auto overwritten=bundle; fixture::put32(overwritten[3].memory,0x330,0x00a02021);
            bool rejected=false;
            try { nt::iop_codegen::ir::discover_bundle(overwritten,bundle_roots); } catch (const std::runtime_error&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Hoisted table ignored an overwritten index in the branch delay slot");
        }
        {
            const auto discovered=nt::iop_codegen::ir::discover_bundle(bundle,bundle_roots);
            if (!discovered.open_indirect_sites.contains(0x11294))
                throw std::runtime_error("Callback candidate recovery narrowed an unbounded native slot");
            const auto& received=discovered.instructions.at(0x11214).targets;
            if (std::find(received.begin(),received.end(),0x12080)==received.end())
                throw std::runtime_error("Registry lost a callback constructed and passed by its compiled client");
            const auto& targets=discovered.instructions.at(0x11118).targets;
            if (!discovered.instructions.contains(0x12040) || std::find(targets.begin(),targets.end(),0x12040)==targets.end())
                throw std::runtime_error("Registry client callback was not compiled in its owning module");
        }
        {
            auto broken=bundle; fixture::put32(broken[3].memory,0x300,0);
            std::ostringstream partial; bool rejected=false;
            try { nt::iop_codegen::emit_bundle(partial,broken,bundle_roots); }
            catch (const std::runtime_error&) { rejected=true; }
            if (!rejected || !partial.str().empty()) throw std::runtime_error("Malformed hoisted table emitted native code");
        }
        code.str(""); code.clear(); nt::iop_codegen::emit_bundle(code,bundle,bundle_roots);
        {
            auto broken=bundle;
            fixture::put32(broken[0].memory,0,0xfc000000);
            fixture::put32(broken[1].memory,0,0xfc000000);
            std::ostringstream partial; std::string diagnostic;
            try { nt::iop_codegen::emit_bundle(partial,broken); }
            catch (const std::runtime_error& error) { diagnostic=error.what(); }
            if (!partial.str().empty() || diagnostic.find("module 0 base=0x00001000")==std::string::npos ||
                diagnostic.find("module 1 base=0x0000a000")==std::string::npos)
                throw std::runtime_error("Bundle conversion concealed another module failure or emitted partial output");
        }
        for (const auto target : {0U,0xa501U,0xb800U}) {
            auto broken=bundle; fixture::put32(broken[2].memory,0x480,target);
            std::ostringstream partial; bool failed=false;
            try { nt::iop_codegen::emit_bundle(partial,broken,bundle_roots); }
            catch (const std::runtime_error&) { failed=true; }
            if (!failed || !partial.str().empty()) throw std::runtime_error("Invalid dispatch table emitted native code");
        }
        for (unsigned invalid=0;invalid<3;++invalid) {
            auto broken=bundle;
            if (invalid==0) broken[1]=module;
            if (invalid==1) { broken[1].memory[40]=2; broken[1].memory[41]=2; }
            if (invalid==2) broken[1].memory[3]=0xfc;
            std::ostringstream partial; bool failed_bundle=false;
            try { nt::iop_codegen::emit_bundle(partial,broken); }
            catch (const std::runtime_error&) { failed_bundle=true; }
            if (!failed_bundle || !partial.str().empty()) throw std::runtime_error("Invalid bundle emitted partial native code");
        }
        // Earlier owners must be revisited when a later module discovers a callback.
        const std::array reversed{provider,module,dispatch,factories,pointers,display,registry_client,sound,usb,usb_client,hoisted_table,network_services,dev9,fixed_pool,edge_table,rpc_client};
        std::ostringstream reverse_code;
        nt::iop_codegen::emit_bundle(reverse_code,reversed,bundle_roots);
        for (const auto target : {0xa061U,0xa080U,0xb800U}) {
            auto broken=bundle;
            fixture::put32(broken[0].memory,0xb4c,0x34060000U|target);
            std::ostringstream partial; bool rejected=false;
            try { nt::iop_codegen::emit_bundle(partial,broken,bundle_roots); }
            catch (const std::runtime_error&) { rejected=true; }
            if (!rejected || !partial.str().empty()) throw std::runtime_error("Invalid callback owner emitted native code");
        }
        {
            auto broken=bundle; fixture::put32(broken[1].memory,0x48,0x4000);
            std::ostringstream partial; bool rejected=false;
            try { nt::iop_codegen::emit_bundle(partial,broken,bundle_roots); }
            catch (const std::runtime_error&) { rejected=true; }
            if (!rejected || !partial.str().empty()) throw std::runtime_error("Invalid descriptor entry emitted native code");
        }
        const auto generated=reverse_code.str();
        if (generated.find("nt::iop::run")!=std::string::npos || generated.find("iop_cpu.hpp")!=std::string::npos)
            throw std::runtime_error("Generated code references interpreter execution");
        nt::write_file(argv[1],nt::ByteView(reinterpret_cast<const std::uint8_t*>(generated.data()),generated.size()));
        if (argc==4) {
            auto large=module;
            constexpr unsigned first=0x8000, count=1102;
            large.memory.resize(first-large.base+count*4);
            large.initialized_size=static_cast<std::uint32_t>(large.memory.size());
            const auto put=[&](unsigned pc,std::uint32_t word) {
                for (unsigned byte=0;byte<4;++byte) large.memory[pc-large.base+byte]=static_cast<std::uint8_t>(word>>(byte*8));
            };
            for (unsigned index=0;index<count-2;++index) put(first+index*4,0x25080001);
            put(0x83fc,0x10000001); // Branch and delay slot straddle native functions.
            put(0x87fc,0x8c820000); // Pending load straddles another boundary.
            put(0x8800,0x00401821);
            put(first+(count-2)*4,0x03e00008); put(first+(count-1)*4,0);
            std::vector<std::uint32_t> roots=bundle_roots; roots.push_back(first);
            const std::array large_bundle{large,provider,dispatch,factories,pointers,display,registry_client,sound,usb,usb_client,hoisted_table,network_services,dev9,fixed_pool,edge_table,rpc_client};
            std::ostringstream llvm; nt::iop_codegen::emit_bundle(llvm,large_bundle,roots);
            const auto text=llvm.str();
            nt::write_file(argv[3],nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        }
        if (argc>=3) {
            nt::Bytes irx(544);
            const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
            std::copy(std::begin(identity),std::end(identity),irx.begin());
            const auto put=[&](unsigned at,std::uint32_t value) {
                for (unsigned byte=0;byte<4;++byte) irx[at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
            };
            irx[16]=0x80; irx[17]=0xff; irx[18]=8; irx[20]=1;
            put(28,52); put(32,84); irx[40]=52; irx[42]=32; irx[44]=1; irx[46]=40; irx[48]=1;
            put(52,1); put(56,128); put(68,416); put(72,448); put(76,7); put(80,16);
            // Synthetic module for the project's fixed 0x1000 relocation base.
            // Allocate native memory, fill/read it, free it, and return 43.
            // Allocation failure reaches an explicit BREAK rather than using null.
            constexpr std::uint32_t entry[]{0x03e08021,0x24041100,0x0c000455,0,0x14400016,0,0x24040000,0x24050001,0x0c000435,0,
                0x10400010,0,0x00408821,0x00402021,0x2405002b,0x24060001,0x0c000425,0,
                0x92320000,0,0x02202021,0x0c000437,0,0x02401021,0x0200f821,0x03e00008,0,13};
            for (unsigned index=0;index<std::size(entry);++index) put(128+index*4,entry[index]);
            put(256,0x41e00000); put(264,0x104);
            std::copy(std::begin(library),std::end(library),irx.begin()+268);
            put(276,0x03e00008); put(280,0x2400000e);
            put(320,0x41e00000); put(328,0x102);
            std::copy(std::begin(heap_name),std::end(heap_name),irx.begin()+332);
            put(340,0x03e00008); put(344,0x24000004);
            put(348,0x03e00008); put(352,0x24000005);
            put(384,0x41c00000); put(392,0x100);
            std::copy(std::begin(name),std::end(name),irx.begin()+396); put(404,0x1180);
            put(448,0x41e00000); put(456,0x103);
            std::copy_n(loader_name,8,irx.begin()+460);
            put(468,0x03e00008); put(472,0x24000006);
            put(512,0x24820001); put(516,0x03e00008);
            nt::write_file(argv[2],irx);
            auto archive_irx=irx;
            std::fill(archive_irx.begin()+128,archive_irx.end(),0);
            fixture::put32(archive_irx,128,0xfc000000); // Must not execute a replaced system-module entry.
            fixture::put32(archive_irx,384,0x41c00000); fixture::put32(archive_irx,392,0x201);
            std::copy(std::begin(archive_library),std::end(archive_library),archive_irx.begin()+396);
            for (unsigned i=0;i<6;++i) fixture::put32(archive_irx,404+i*4,0x7000);
            nt::write_file(std::string(argv[2])+".romdrv.irx",archive_irx);
            // A separate client starts at 0x3000 and calls the first module's export.
            auto client_irx=nt::Bytes(256);
            std::copy_n(irx.begin(),128,client_irx.begin());
            const auto client_put=[&](unsigned at,std::uint32_t value) {
                for (unsigned byte=0;byte<4;++byte) client_irx[at+byte]=static_cast<std::uint8_t>(value>>(byte*8));
            };
            client_put(68,128); client_put(72,128);
            constexpr std::uint32_t client_entry[]{0x03e08021,0x2404002a,0x0c000c15,0,0x0200f821,0x03e00008,0};
            for (unsigned index=0;index<std::size(client_entry);++index) client_put(128+index*4,client_entry[index]);
            client_put(192,0x41e00000); client_put(200,0x100);
            std::copy(std::begin(name),std::end(name),client_irx.begin()+204);
            client_put(212,0x03e00008); client_put(216,0x24000000);
            nt::write_file(std::string(argv[2])+".client.irx",client_irx);
            nt::Bytes disc_client(640);
            std::copy_n(irx.begin(),128,disc_client.begin());
            const auto disc_put=[&](unsigned at,std::uint32_t value) { fixture::put32(disc_client,at,value); };
            disc_put(68,512); disc_put(72,512);
            constexpr std::uint32_t disc_entry[]{0x03e08021,0x24045100,0x24050001,0x0c00142d,0,
                0x0440000d,0,0x00408821,0x00402021,0x24055140,0x24060001,0x0c001431,0,0x90b20000,0,
                0x02202021,0x0c00142f,0,0x02401021,0x0200f821,0x03e00008,0};
            for (unsigned index=0;index<std::size(disc_entry);++index) disc_put(128+index*4,disc_entry[index]);
            disc_put(288,0x41e00000); disc_put(296,0x104);
            std::copy(std::begin(file_name),std::end(file_name),disc_client.begin()+300);
            for (unsigned index=0;index<3;++index) { disc_put(308+index*8,0x03e00008); disc_put(312+index*8,0x24000004+index); }
            constexpr char disc_path[]=R"(cdrom0:\SYSTEM.CNF;1)";
            std::copy(std::begin(disc_path),std::end(disc_path),disc_client.begin()+384);
            nt::write_file(std::string(argv[2])+".disc.irx",disc_client);
            const nt::Bytes payload{43,7,9};
            nt::write_file(std::string(argv[2])+".iso",fixture::single_file_iso("SYSTEM.CNF;1",payload));
        }
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
