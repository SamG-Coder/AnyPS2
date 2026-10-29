#include <nt/ee_codegen.hpp>
#include "disc_fixture.hpp"
#include "ee_batch_fixture.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
int main(int argc,char** argv) {
    if (argc!=2 && argc!=3) return 1;
    // Original scalar program: word sign extension, 64-bit addition and syscall.
    const std::uint32_t words[]{0x3c088000,0x2509ffff,0x652a0001,0x350b0001,0x000b6102,0x000b68c3,0x010a702d,0x010b7827,0x24000001,0x0000000c,
        0x24080003,0x24090000,0x2508ffff,0x1500fffe,0x25290001,0x0c000414,0x240a0007,0x0000000c,
        0x00800008,0x24040000,0x03e05821,0x03e00008,0x254a0001,
        0x00802009,0x258c0001,0x0000000c,0x10850002,0x25ad0001,0x0000000c,0x0000000c,0x08000409,0x25ce0001,
        0xfc880000,0x80890000,0x908a0000,0x848b0000,0x948c0000,0x8c8d0000,0x9c8e0000,0xdc8f0000,
        0xa0880008,0xa488000a,0xac88000c,0x0000000c,
        0x10000002,0xac880000,0x0000000c,0x0000000c,0x8c800000,0x0000000c};
    std::vector<nt::r5900::Instruction> code;
    for (auto word:words) code.push_back(nt::r5900::decode(0x1000+static_cast<std::uint32_t>(code.size())*4,word));
    const auto append=[&](std::uint32_t word) {
        code.push_back(nt::r5900::decode(0x1000+static_cast<std::uint32_t>(code.size())*4,word));
    };
    for (const auto destination : {10U,8U,9U,0U}) {
        append((28U<<26)|(8U<<21)|(9U<<16)|(destination<<11)|(16U<<6)|40U);
        append(12); // syscall
    }
    append((28U<<26)|(9U<<16)|(10U<<11)|(16U<<6)|40U); append(12);
    for (const auto word : {0x01000011U,0x01200013U,0x00005010U,0x00005812U,12U,
                            0x00000011U,0x00000013U,0x00000010U,0x00000012U,12U}) append(word);
    for (const auto word : {0x71000011U,0x71200013U,0x70005010U,0x70005812U,12U,
                            0x70000011U,0x70000013U,0x70000010U,0x70000012U,12U}) append(word);
    for (const auto selector : {24U,25U}) {
        append((1U<<26)|(8U<<21)|(selector<<16)|0xfffaU);
        append((10U<<11)|40U); append(12);
    }
    append((1U<<26)|(25U<<16)); append(40U); append(12);
    for (const auto word : {0x44881000U,0x44091000U,0x44801800U,0x44001000U,12U}) append(word);
    for (const auto word : {0x46031018U,12U,0x46031100U,12U,0x46031101U,12U,
                            0x10000002U,0x46031018U,12U,12U,0x46031080U,12U}) append(word);
    append(15); append((16U<<6)|15U); append(12);
    for (const auto word : {0x44c8f800U,0x4449f800U,12U,0x44c0f800U,0x4440f800U,12U}) append(word);
    for (const auto word : {0x7c880000U,0x78890000U,0x7c800010U,0x78800000U,12U,0x78840000U,12U}) append(word);
    for (const auto word : {0x2403003cU,12U,0x0040e821U,0x2403007fU,12U}) append(word);
    for (const auto word : {0x2403003dU,12U,0x00408021U,0x2403003eU,12U,0x00408821U,0x2403007fU,12U}) append(word);
    auto invalid=code; invalid[3]=nt::r5900::decode(0x100c,0x4c000000);
    std::ostringstream rejected; bool failed=false;
    try { nt::ee_codegen::emit_llvm(rejected,invalid); } catch (const std::runtime_error&) { failed=true; }
    if (!failed || !rejected.str().empty()) throw std::runtime_error("Unsupported EE code emitted partial output");
    invalid=code;
    invalid[0]=nt::r5900::decode(0x1000,0x4c000000);
    invalid[1]=nt::r5900::decode(0x1004,0x4c000001);
    invalid[2]=nt::r5900::decode(0x1008,0x74000000);
    std::string inventory;
    try { nt::ee_codegen::emit_llvm(rejected,invalid); }
    catch (const std::runtime_error& error) { inventory=error.what(); }
    if (inventory.find("families (2)")==std::string::npos ||
        inventory.find("primary=19): 2 occurrence(s)")==std::string::npos ||
        inventory.find("primary=29): 1 occurrence(s)")==std::string::npos || !rejected.str().empty())
        throw std::runtime_error("Unsupported-family inventory was incomplete");
    invalid=code; invalid[2].rs=31; failed=false;
    try { nt::ee_codegen::emit_llvm(rejected,invalid); } catch (const std::invalid_argument&) { failed=true; }
    if (!failed || !rejected.str().empty()) throw std::runtime_error("Inconsistent decoded EE code accepted");
    for (const auto word : {0x08000400U,0x0000000cU,0x42000018U}) {
        invalid=code; invalid[4]=nt::r5900::decode(invalid[4].pc,0x08000400);
        invalid[5]=nt::r5900::decode(invalid[5].pc,word); failed=false;
        try { nt::ee_codegen::emit_llvm(rejected,invalid); } catch (const std::invalid_argument&) { failed=true; }
        if (!failed || !rejected.str().empty()) throw std::runtime_error("Unsupported delay slot emitted output");
    }
    invalid=code; invalid[0]=nt::r5900::decode(invalid[0].pc,(1U<<6)|15U); failed=false;
    try { nt::ee_codegen::emit_llvm(rejected,invalid); } catch (const std::invalid_argument&) { failed=true; }
    if (!failed || !rejected.str().empty()) throw std::runtime_error("Reserved SYNC mode emitted output");
    for (const auto word : {0x44c80000U,0x44490000U}) {
        invalid=code; invalid[0]=nt::r5900::decode(invalid[0].pc,word); failed=false;
        try { nt::ee_codegen::emit_llvm(rejected,invalid); } catch (const std::invalid_argument&) { failed=true; }
        if (!failed || !rejected.str().empty()) throw std::runtime_error("Unsupported FCR selector emitted output");
    }
    // EI, DI, and an EI in a branch delay slot: original interrupt-gate fixture.
    for (const auto word : {0x42000038U,0x24160007U,0x42000039U,12U,
                            0x10000002U,0x42000038U,12U,0x24160009U,12U}) append(word);
    for (const auto word : {0x0109502aU,0x0109582bU,0x290cffffU,0x2d0dffffU,12U}) append(word);
    for (const auto word : {0x0109500aU,0x0109580bU,0x0109480aU,0x0108000bU,12U}) append(word);
    // Each conditional branch has a potentially faulting delay load.
    for (const auto branch : {0x51090002U,0x55090002U,0x19000002U,0x1d000002U,0x05000002U,0x05010002U,
                              0x59000002U,0x5d000002U,0x05020002U,0x05030002U}) {
        append(branch); append(0x8c8a0000); append(12); append(0x240b0009); append(12);
    }
    for (const auto fn : {56U,58U,59U,60U,62U,63U,20U,22U,23U,4U,6U,7U}) {
        const bool variable=fn<32;
        append((variable ? 8U<<21 : 0U)|(9U<<16)|(10U<<11)|(variable ? 0U : 31U<<6)|fn);
        append(12);
    }
    for (const auto word : {0xe480fffcU,0xc483fffcU,12U,0xc480fffcU,12U}) append(word);
    for (const auto fn : {48U,50U,52U,54U}) { append(0x46031000U|fn); append(12); }
    for (const auto selector : {0U,1U,2U,3U}) {
        append(0x45000002U|(selector<<16)); append(0x240a0007); append(12); append(0x240b0009); append(12);
    }
    for (const auto word : {0x4848e000U,12U,0x4849e001U,12U,0x4840e001U,12U,
                            0x10000002U,0x4848e001U,12U,12U}) append(word);
    for (const auto word : {0x48c8e000U,12U,0x48c8e001U,12U,0x48c0e001U,12U}) append(word);
    for (const auto word : {0xd8810000U,0xf8810010U,0xf8800020U,0xd8800030U,12U}) append(word);
    for (const auto word : {0x460010c6U,0x46001105U,0x46001147U,0x46001087U,12U}) append(word);
    for (const auto word : {0x4088c800U,0x4089c801U,0x408ac803U,0x400bc800U,0x400cc801U,0x400dc803U,12U,
                            0x4080c801U,0x4080c803U,12U,0x4088c802U,12U}) append(word);
    for (const auto fn : {24U,25U,26U,27U}) { append((8U<<21)|(9U<<16)|(10U<<11)|fn); append(12); }
    for (const auto word : {0x000001cdU,0x10000002U,0x000001cdU,12U,12U}) append(word);
    for (const auto word : {0x46031102U,12U,0x46031082U,12U}) append(word);
    for (const auto fn : {44U,40U,4U,1U}) {
        append((18U<<26)|(31U<<21)|(2U<<16)|(1U<<11)|(3U<<6)|fn); append(12);
    }
    append((18U<<26)|(26U<<21)|(2U<<16)|(1U<<11)|(1U<<6)|44U); append(12);
    append((18U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|44U); append(12);
    for (unsigned rotate=0;rotate<2;++rotate) for (unsigned mask=0;mask<16;++mask) {
        append((18U<<26)|((16U|mask)<<21)|((rotate ? 1U : 2U)<<16)|(1U<<11)|(rotate ? 0x33dU : 0x33cU)); append(12);
    }
    append((18U<<26)|(31U<<21)|(2U<<16)|0x33cU); append(12);
    append((18U<<26)|(31U<<21)|(1U<<11)|0x33cU); append(12);
    for (const auto word : {0x68880000U,12U,0x6c880000U,12U,0x68880007U,0x6c880000U,12U,0x68800000U,12U}) append(word);
    for (const auto word : {0xb0880000U,12U,0xb4880000U,12U,0xb0880007U,0xb4880000U,12U,0xb4800000U,12U}) append(word);
    for (const auto selector : {12U,14U,30U}) {
        append(0x40880000U|(selector<<11)); append(0x40090000U|(selector<<11)); append(12);
    }
    append(0x40806000U); append(12);
    for (const auto destination : {9U,8U,0U}) { append((28U<<26)|(8U<<21)|(destination<<11)|4U); append(12); }
    for (const auto word : {0x01095020U,0x01095022U,0x210affffU,0x0109502cU,0x0109502eU,0x610affffU}) { append(word); append(12); }
    for (const auto word : {0x01090020U,12U,0x10000002U,0x01095022U,12U,12U}) append(word);
    for (unsigned variant=0;variant<3;++variant) for (const auto destination : {10U,8U,9U,0U}) {
        append((28U<<26)|(8U<<21)|(9U<<16)|(destination<<11)|((variant==2 ? 27U : 14U)<<6)|(variant==0 ? 9U : 41U)); append(12);
    }
    append((28U<<26)|(10U<<11)|(14U<<6)|9U); append(12);
    for (const auto selector : {0U,1U,2U,3U}) {
        append((28U<<26)|(8U<<21)|(9U<<16)|(8U<<11)|((selector>=2 ? 19U : 18U)<<6)|((selector&1) ? 41U : 9U)); append(12);
    }
    for (const auto selector : {10U,6U,2U}) for (const auto destination : {10U,8U,9U,0U}) {
        append((28U<<26)|(8U<<21)|(9U<<16)|(destination<<11)|(selector<<6)|40U); append(12);
    }
    append((28U<<26)|(10U<<11)|(10U<<6)|40U); append(12);
    while (code.size()<512) append(0);
    constexpr std::uint32_t large_base=0x100000;
    for (unsigned index=0;index<4608;++index) {
        auto word=index ? 0x26940001U : 0x24140000U;
        if (index==511) word=(2U<<26)|((large_base+1024*4)>>2);
        if (index==512) word=0x26940007;
        code.push_back(nt::r5900::decode(large_base+index*4,word));
    }
    code.push_back(nt::r5900::decode(large_base+4608*4,12));
    code.push_back(nt::r5900::decode(large_base+4609*4,0x00800008));
    code.push_back(nt::r5900::decode(large_base+4610*4,0x26b50001));
    std::uint32_t extra_pc=0x200000;
    const auto extra=[&](std::uint32_t word) { code.push_back(nt::r5900::decode(extra_pc,word)); extra_pc+=4; };
    for (const auto selector : {31U,30U}) for (const auto destination : {10U,9U,0U}) {
        extra((28U<<26)|(9U<<16)|(destination<<11)|(selector<<6)|8U); extra(12);
    }
    for (unsigned kind=0;kind<3;++kind) for (const auto selector : {26U,22U,18U}) for (const auto destination : {10U,8U,9U,0U}) {
        extra((28U<<26)|(8U<<21)|(9U<<16)|(destination<<11)|((selector+(kind==0 ? 1U : 0U))<<6)|(kind==2 ? 40U : 8U)); extra(12);
    }
    for (const auto fn : {24U,25U,26U,27U}) { extra((28U<<26)|(8U<<21)|(9U<<16)|(10U<<11)|fn); extra(12); }
    for (const auto word : {0x88880000U,12U,0x98880000U,12U,0xa8880000U,12U,0xb8880000U,12U,
                            0x88880003U,0x98880000U,12U,0xa8880003U,0xb8880000U,12U}) extra(word);
    for (const auto word : {0x88800000U,0x98800000U,0xa8800000U,0xb8800000U}) { extra(word); extra(12); }
    for (std::size_t index=0;index<ee_batch::packed.size();++index) {
        const auto fixture=ee_batch::packed[index];
        for (unsigned variant=0;variant<4;++variant) {
            const auto pc=ee_batch::packed_pc(index,variant);
            const auto word=(fixture.word&~(31U<<11))|(ee_batch::destinations[variant]<<11);
            const auto decoded=nt::r5900::decode(pc,word);
            if (nt::r5900::name(decoded.op)!=fixture.name) throw std::runtime_error("Packed fixture decoded incorrectly: "+std::string(fixture.name));
            code.push_back(decoded); code.push_back(nt::r5900::decode(pc+4,12));
        }
    }
    for (std::size_t index=0;index<ee_batch::scalar.size();++index) {
        const auto pc=ee_batch::scalar_base+static_cast<std::uint32_t>(index)*8;
        code.push_back(nt::r5900::decode(pc,ee_batch::scalar[index].word));
        code.push_back(nt::r5900::decode(pc+4,12));
    }
    for (std::size_t index=0;index<ee_batch::vector.size();++index) {
        const auto fixture=ee_batch::vector[index];
        const bool transfer=fixture.name.starts_with("qm");
        const bool binary=fixture.name.starts_with("vmul") || fixture.name.starts_with("vmax") || fixture.name.starts_with("vmini");
        for (unsigned mask=0;mask<4;++mask) for (unsigned variant=0;variant<4;++variant) {
            auto word=fixture.word;
            if (transfer) {
                if (fixture.name.starts_with("qmfc")) {
                    word=(word&~((31U<<11)|(31U<<16)))|
                         ((mask&1 ? 0U : 2U)<<11)|(std::array{8U,2U,3U,0U}[variant]<<16);
                } else {
                    word=(word&~((31U<<11)|(31U<<16)))|
                         (std::array{2U,8U,3U,0U}[variant]<<11)|((mask&1 ? 0U : 8U)<<16);
                }
            } else if (fixture.name!="vnop") {
                word=(word&~(15U<<21))|(std::array{15U,10U,5U,0U}[mask]<<21);
                const auto shift=binary ? 6 : 16;
                word=(word&~(31U<<shift))|(std::array{4U,2U,3U,0U}[variant]<<shift);
            }
            const auto pc=ee_batch::vector_pc(index,mask,variant);
            code.push_back(nt::r5900::decode(pc,word)); code.push_back(nt::r5900::decode(pc+4,12));
        }
    }
    for (unsigned operation=0;operation<4;++operation) {
        const auto pc=ee_batch::branch_base+operation*32;
        // Both paths capture the link in a delay slot; likely-false skips it.
        for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,5>{{
            {0,(1U<<26)|(8U<<21)|((16U+operation)<<16)|3U},
            {4,0x03e0502dU},{8,12U},{12,0U},{16,12U}}})
            code.push_back(nt::r5900::decode(pc+offset,word));
    }
    for (unsigned form=0;form<2;++form) for (unsigned operation=0;operation<6;++operation) {
        const auto pc=ee_batch::trap_base+(form*6+operation)*8;
        const auto word=form ? (1U<<26)|(8U<<21)|(ee_batch::trap_immediates[operation]<<16)|0xffffU :
                               (8U<<21)|(9U<<16)|ee_batch::trap_functions[operation];
        code.push_back(nt::r5900::decode(pc,word)); code.push_back(nt::r5900::decode(pc+4,12));
    }
    code.push_back(nt::r5900::decode(ee_batch::prefetch_pc,(51U<<26)|(8U<<21)|0xffffU));
    code.push_back(nt::r5900::decode(ee_batch::prefetch_pc+4,12));
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,4>{{
        {0,0x10000002U},{4,(8U<<21)|(9U<<16)|52U},{8,12U},{12,12U}}})
        code.push_back(nt::r5900::decode(ee_batch::trap_delay_pc+offset,word));
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,7>{{
        {0,0x24820007U},{4,0x03e00008U},{8,0U},
        {16,0x24030061U},{20,12U},{24,0x03e00008U},{28,0U}}})
        code.push_back(nt::r5900::decode(0x350000+offset,word));
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,4>{{
        {0,0xac880000U},{4,12U},{8,0x8c880000U},{12,12U}}})
        code.push_back(nt::r5900::decode(0x360000+offset,word));
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,7>{{
        {0,0x8f880000U},{4,0x00084100U},{8,0x01054021U},{12,0xaf880000U},
        {16,0x00a0102dU},{20,0x03e00008U},{24,0U}}})
        code.push_back(nt::r5900::decode(0x365000+offset,word));
    nt::ee_codegen::CodePayload payload{0x370000,{}};
    for (const auto word : {0x24020007U,0x03e00008U,0U}) {
        code.push_back(nt::r5900::decode(payload.address+static_cast<std::uint32_t>(payload.bytes.size()),word));
        for (unsigned byte=0;byte<4;++byte) payload.bytes.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
    }
    code.push_back(nt::r5900::decode(0x380000,0x080e0000U));
    code.push_back(nt::r5900::decode(0x380004,0U));
    code.push_back(nt::r5900::decode(0x390000,0xaf840000U));
    code.push_back(nt::r5900::decode(0x390004,0x24030023U));
    code.push_back(nt::r5900::decode(0x390008,12U));
    code.push_back(nt::r5900::decode(0x3a0000,12));
    for (unsigned index=0;const auto reg:{0U,2U,3U,5U,10U}) {
        code.push_back(nt::r5900::decode(0x3b0000+index*8,0x40880000U|(reg<<11)));
        code.push_back(nt::r5900::decode(0x3b0004+index*8,0x40090000U|(reg<<11)));
        ++index;
    }
    code.push_back(nt::r5900::decode(0x3b0028,12));
    for (unsigned index=0;const auto word:{0x42000001U,0x42000002U,0x42000006U,0x42000008U}) {
        code.push_back(nt::r5900::decode(0x3b0100+index*4,word)); ++index;
    }
    code.push_back(nt::r5900::decode(0x3b0110,12));
    const std::array payloads{payload};
    std::ostringstream generated; nt::ee_codegen::emit_llvm(generated,code,{},payloads);
    if (generated.str().find("fence seq_cst")==std::string::npos) throw std::runtime_error("Native SYNC ordering was omitted");
    const auto text=generated.str();
    unsigned functions=0;
    for (std::size_t start=0;(start=text.find("define internal i32 @nt_ee_part_",start))!=std::string::npos;) {
        const auto end=text.find("\n}",start);
        unsigned blocks=0;
        for (auto block=text.find("\nblock",start);block<end;block=text.find("\nblock",block+1)) ++blocks;
        if (!blocks || blocks>nt::ee_codegen::function_instruction_limit) throw std::runtime_error("EE function instruction bound violated");
        ++functions; start=end;
    }
    if (functions<10) throw std::runtime_error("Large EE program was not partitioned");
    std::ofstream output(argv[1]); output<<text;
    if (argc==3) {
        nt::Bytes elf(0x184,0);
        const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
        std::copy(std::begin(identity),std::end(identity),elf.begin());
        const auto word=[&](std::size_t at,std::uint32_t value) { fixture::put32(elf,at,value); };
        word(16,0x00080002); word(20,1); word(24,0x1000); word(28,52);
        word(40,0x00200034); word(44,2);
        word(52,1); word(56,0x100); word(60,0x1000); word(68,0x80); word(72,0x80); word(76,5); word(80,4);
        word(84,1); word(88,0x180); word(92,0x2000); word(100,4); word(104,32); word(108,6); word(112,4);
        const std::uint32_t entry[]{0x24042000,0x8c880000,0x8c890004,0x25080001,0xac880004,0x0c000410,0x25290001,0x0000000c};
        for (unsigned i=0;i<std::size(entry);++i) word(0x100+i*4,entry[i]);
        word(0x120,0xffffffff); // Unreachable after the syscall: must not be translated.
        word(0x140,0x8c820004); word(0x144,0x03e00008); word(0x148,0x00491021); word(0x180,41);
        nt::write_file(argv[2],elf);
        const auto original=elf;
        const std::uint32_t startup[]{0x24040000,0x24054000,0x24061000,0x24072000,0x24081040,
            0x2403003c,12,0x0040e821,0x24043000,0x2405ffff,0x2403003d,12,
            0x00408021,0x2403003e,12,0x2403007f,12};
        for (unsigned i=0;i<std::size(startup);++i) word(0x100+i*4,startup[i]);
        nt::write_file(std::string(argv[2])+".startup",elf);
        elf=original;
        word(92,0x1000); nt::write_file(std::string(argv[2])+".overlap",elf);
        word(92,0x2000); word(0x100,0x4c000000); nt::write_file(std::string(argv[2])+".unsupported",elf);
    }
    return output ? 0 : 1;
}
