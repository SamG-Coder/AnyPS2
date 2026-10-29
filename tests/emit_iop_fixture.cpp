#include <nt/iop_codegen.hpp>
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
        module.memory.resize(0x52c);
        const auto query_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[0x500+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        query_word(0,0x41e00000); query_word(8,0x103);
        std::copy_n(loader_name,8,module.memory.begin()+0x50c);
        query_word(20,0x03e00008); query_word(24,0x2400000b);
        query_word(28,0x03e00008); query_word(32,0x2400000c);
        module.memory.resize(0x63c);
        const auto file_word=[&](unsigned at,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[0x600+at+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        file_word(0,0x41e00000); file_word(8,0x104);
        constexpr char file_name[]="ioman";
        std::copy(std::begin(file_name),std::end(file_name),module.memory.begin()+0x60c);
        const unsigned file_ordinals[]{4,5,6,8};
        for (unsigned index=0;index<4;++index) { file_word(20+index*8,0x03e00008); file_word(24+index*8,0x24000000|file_ordinals[index]); }
        module.memory.resize(0x1e00);
        const auto irq_word=[&](unsigned address,std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) module.memory[address-module.base+byte]=static_cast<std::uint8_t>(word>>(byte*8));
        };
        irq_word(0x1700,0x41e00000); irq_word(0x1708,0x102);
        constexpr char irq_name[]="intrman";
        std::copy(std::begin(irq_name),std::end(irq_name),module.memory.begin()+0x70c);
        const unsigned irq_ordinals[]{4,5,6,7,8,9,17,18,23};
        for (unsigned index=0;index<9;++index) { irq_word(0x1714+index*8,0x03e00008); irq_word(0x1718+index*8,0x24000000|irq_ordinals[index]); }
        // Handler is discovered only through the constant registration argument.
        constexpr std::uint32_t handler[]{0x03e08021,0x0c0005d5,0,0xac820004,0x8c880000,0,0x25080001,0xac880000,
            0x24020001,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(handler);++index) irq_word(0x1a00+index*4,handler[index]);
        irq_word(0x1a40,0x24020000); irq_word(0x1a44,0x03e00008); irq_word(0x1a50,0x0000008d);
        constexpr std::uint32_t registration[]{0x03e08021,0x24040003,0x24050002,0x24061a00,0x3407c000,
            0x0c0005c5,0,0x0200f821,0x03e00008,0};
        for (unsigned index=0;index<std::size(registration);++index) irq_word(0x1b00+index*4,registration[index]);
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
        const unsigned thread_ordinals[]{20,24,25,26,27,28,4,5,6,8,9,7,41,34,35,36,37,38,14,22,33};
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
        irq_word(0x2c58,0x2c80);
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
        module.initialized_size=static_cast<std::uint32_t>(module.memory.size());
        std::vector<std::uint32_t> bundle_roots=entries;
        bundle_roots.insert(bundle_roots.end(),{0x13ec,0x1418,0x1420,0x1514,0x151c,0x1614,0x161c,0x1624,0x162c,0x171c,0x1724,0x172c,0x1734,0x173c,0x1744,0x174c,0x1754,0x1a40,0x1a50,0x1b00,0x1b40,0x1c00,0x1d00,0x1d80,0x1dc0,0x1e00,0x1e40,0x1f20,0x2140,0x2180,0x21c0,0x2314,0x2354,0x2394,0x239c,0x23a4,0x23ac,0x23b4,0x23bc,0x23c4,0x23cc,0x23d4,0x23dc,0x23e4,0x23ec,0x2814,0x2824,0x2840,0x2ba0,0x2414,0x241c,0x242c,0x2434,0x243c,0x2444,0x244c,0x2454,0x245c,0x2464,0x246c,0x2480,0x251c,0x2524,0x252c,0x2534,0x253c,0x2544,0x254c,0x2580,0x2600,0x2680,0x2714,0x271c,0x2724,0x272c,0x2734,0x273c,0x2744,0x274c,0x2794,0x279c,0x27a4,0x27ac,0x27b4,0x27bc,0x27c4,0x27cc});
        for (unsigned index=0;index<std::size(thread_ordinals);++index)
            if (thread_ordinals[index]!=4 && thread_ordinals[index]!=35) bundle_roots.push_back(0x1914+index*8);
        for (unsigned index=0;index<std::size(event_ordinals);++index) bundle_roots.push_back(0x1814+index*8);
        for (unsigned index=0;index<std::size(sif_ordinals);++index) bundle_roots.push_back(0x2214+index*8);
        const std::array bundle{module,provider};
        code.str(""); code.clear(); nt::iop_codegen::emit_bundle(code,bundle,bundle_roots);
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
        const std::array reversed{provider,module};
        std::ostringstream reverse_code;
        nt::iop_codegen::emit_bundle(reverse_code,reversed,bundle_roots);
        for (const auto target : {0xa061U,0xa080U,0xb000U}) {
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
            const std::array large_bundle{large,provider};
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
                0x00408821,0x00402021,0x24055140,0x24060001,0x0c001431,0,0x90b20000,0,
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
