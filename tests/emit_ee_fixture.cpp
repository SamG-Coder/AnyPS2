#include <nt/ee_codegen.hpp>
#include <nt/analysis.hpp>
#include "disc_fixture.hpp"
#include "ee_batch_fixture.hpp"
#include "vector_section_fixture.hpp"
#include "object_callback_fixture.hpp"
#include "flow_observation_fixture.hpp"
#include "ee_partition_fixture.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <algorithm>
int main(int argc,char** argv) try {
    if (argc!=2 && argc!=3) return 1;
    fixture::verify_incremental_ee_modules();
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
    for (const auto word : {0x2403003cU,12U,0x0040e821U,0x2403007bU,12U}) append(word);
    for (const auto word : {0x2403003dU,12U,0x00408021U,0x2403003eU,12U,0x00408821U,0x2403007bU,12U}) append(word);
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
    // Original interrupt fixture: record interrupted identity and wake a different sleeper.
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,10>{{
        {0,0x2403ffd1U},{4,12U},{8,0xaf820000U},{12,0x10450004U},{16,0U},
        {20,0x00a02021U},{24,0x2403ffccU},{28,12U},{32,0x03e00008U},{36,0x00001021U}}})
        code.push_back(nt::r5900::decode(0x366000+offset,word));
    // Original SBUS callback records its cause and leaves a negative incidental v0.
    for (const auto [offset,word] : std::array<std::pair<unsigned,std::uint32_t>,5>{{
        {0,0x00044080U},{4,0x011c4021U},{8,0xad040000U},{12,0x03e00008U},{16,0x2402ffffU}}})
        code.push_back(nt::r5900::decode(0x367000+offset,word));
    // Original packet callback: record CODE, count calls, release the boundary.
    {
        unsigned pc=0x368000;
        for (const auto word:{0x3c081000U,0x35083c00U,0x8d090080U,0xaf890000U,
                             0x8f8a0004U,0x254a0001U,0xaf8a0004U,0x24090008U,
                             0xad090010U,0x03e00008U,0x00001021U}) {
            code.push_back(nt::r5900::decode(pc,word)); pc+=4;
        }
    }
    nt::ee_codegen::CodePayload payload{0x370000,{}};
    for (const auto word : {0x24020007U,0x03e00008U,0U}) {
        code.push_back(nt::r5900::decode(payload.address+static_cast<std::uint32_t>(payload.bytes.size()),word));
        for (unsigned byte=0;byte<4;++byte) payload.bytes.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
    }
    payload.bytes.insert(payload.bytes.end(),4,0); // Writable state after the returning code.
    code.push_back(nt::r5900::decode(payload.address+12,0)); // Conservative whole-code selection.
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
    for (unsigned index=0;const auto word:{0xaf840000U,0x2403ff88U,12U,0x03e00008U,0U}) {
        code.push_back(nt::r5900::decode(0x3c0000+index*4,word)); ++index;
    }
    for (unsigned index=0;const auto pair:std::array<std::pair<std::uint32_t,std::uint32_t>,4>{{
        {0x7c880000U,0x78890000U},{0xfc880000U,0xdc890000U},{0xac880000U,0x8c890000U},{0xa0880000U,0x80890000U}}}) {
        const auto pc=0x3d0000+index++*16;
        code.push_back(nt::r5900::decode(pc,pair.first)); code.push_back(nt::r5900::decode(pc+4,12));
        code.push_back(nt::r5900::decode(pc+8,pair.second)); code.push_back(nt::r5900::decode(pc+12,12));
    }
    code.push_back(nt::r5900::decode(0x3e0000,0xaf840000));
    code.push_back(nt::r5900::decode(0x3e0004,0x03e00008));
    code.push_back(nt::r5900::decode(0x3e0008,0));
    const std::array payloads{payload};
    code.push_back(nt::r5900::decode(0x3f0000,0x4a000038)); // VCALLMS entry zero.
    code.push_back(nt::r5900::decode(0x3f0004,12));
    code.push_back(nt::r5900::decode(0x3f0010,0x4a000039)); // VCALLMSR via CMSAR0.
    code.push_back(nt::r5900::decode(0x3f0014,12));
    code.push_back(nt::r5900::decode(0x3f0020,0x10000002)); // Launch in an EE delay slot.
    code.push_back(nt::r5900::decode(0x3f0024,0x4a000038));
    code.push_back(nt::r5900::decode(0x3f0028,12));
    code.push_back(nt::r5900::decode(0x3f002c,12));
    {
        unsigned pc=0x400000;
        for (const auto word:{0x7c880000U,0x78890000U,0xac8a0010U,0x8c8b0010U,0xa08c0014U,0x908d0014U,12U}) {
            code.push_back(nt::r5900::decode(pc,word)); pc+=4;
        }
    }
    {
        unsigned pc=0x410000,source=8;
        for (const auto selector:{0U,5U,10U,2U,3U}) {
            code.push_back(nt::r5900::decode(pc,0x40800000U|(source++<<16)|(selector<<11))); pc+=4;
        }
        for (const auto word:{0x0000040fU,0x42000002U,12U}) {
            code.push_back(nt::r5900::decode(pc,word)); pc+=4;
        }
    }
    code.push_back(nt::r5900::decode(0x410020,0x40883000U)); // Native reservation request.
    code.push_back(nt::r5900::decode(0x410024,0x40083000U)); // Readback remains unsupported.
    code.push_back(nt::r5900::decode(0x410028,0x40084800U)); // Count becomes native elapsed-clock read.
    code.push_back(nt::r5900::decode(0x41002c,0x40080800U)); // Random index requires a native binding; never fabricated.
    {
        unsigned pc=0x420010;
        for (const auto word:{0x24040500U,0x34058000U,0x24061000U,0x24072400U,0x24081100U,0x2403003cU,12U,0x2403007bU,12U}) {
            code.push_back(nt::r5900::decode(pc,word)); pc+=4;
        }
    }
    const std::array<nt::ee_codegen::LayoutDeclaration,2> layouts{{
        {0,0xd0000000U,0x87,0xc7}, // Two direct views of synthetic RAM.
        {0,0x13000000U,0x004c0003U,0x004c0041U} // Read-only then inaccessible native port.
    }};
    auto conflicting=layouts; conflicting[1]={0,0xd0000000U,7,0x47};
    failed=false;
    try { std::ostringstream discard; nt::ee_codegen::emit_llvm(discard,code,{},payloads,conflicting); }
    catch (const std::runtime_error&) { failed=true; }
    if (!failed) throw std::runtime_error("Conflicting native aliases accepted");
    conflicting[0]={0,0x13000000U,0x004c0007U,0x004c0041U}; conflicting[1]=layouts[1];
    failed=false;
    try { std::ostringstream discard; nt::ee_codegen::emit_llvm(discard,code,{},payloads,conflicting); }
    catch (const std::runtime_error&) { failed=true; }
    if (!failed) throw std::runtime_error("Conflicting native access permissions accepted");
    std::array<nt::ee_codegen::ExecutableImage,2> images;
    std::array<nt::ee_codegen::CodeRange,3> owned;
    for (unsigned n=0;n<2;++n) {
        const auto address=0x430000U+n*0x100;
        nt::ee_codegen::CodePayload segment{address,{}};
        for (const auto word:{0x24020055U+n,12U}) {
            code.push_back(nt::r5900::decode(address+static_cast<unsigned>(segment.bytes.size()),word));
            for (unsigned b=0;b<4;++b) segment.bytes.push_back(static_cast<std::uint8_t>(word>>(8*b)));
        }
        owned[n]={address,8,n+1}; images[n]={address,n+1,{segment}};
    }
    for (unsigned n=0;const auto function:{28U,29U,30U,31U,22U}) {
        code.push_back(nt::r5900::decode(0x440000+n*8,(17U<<26)|(16U<<21)|(2U<<16)|(1U<<11)|(3U<<6)|function));
        code.push_back(nt::r5900::decode(0x440004+n++*8,12));
    }
    for (unsigned n=0;const auto operation:{0x1bcU,0x1bdU,0x1beU,0x1bfU,0xbcU,0xfcU,0x2bdU,0x2fdU,0x2beU,0xc8U,0xccU,0xe9U,0xedU}) {
        code.push_back(nt::r5900::decode(0x440100+n*8,(18U<<26)|(30U<<21)|(2U<<16)|(1U<<11)|operation));
        code.push_back(nt::r5900::decode(0x440104+n++*8,12));
    }
    code.push_back(nt::r5900::decode(0x440200,(47U<<26)|(4U<<21)|(0x16U<<16)));
    code.push_back(nt::r5900::decode(0x440204,12));
    const std::array<std::uint32_t,21> vector_special{
        (25U<<21)|(2U<<16)|(1U<<11)|0x3bcU, (24U<<21)|(2U<<16)|0x3bdU,
        (25U<<21)|(2U<<16)|(1U<<11)|0x3beU, (16U<<21)|0x3bfU,
        (31U<<21)|(2U<<16)|(1U<<11)|0x1ffU, (30U<<21)|(2U<<16)|(1U<<11)|0x2feU,
        (30U<<21)|(1U<<16)|(2U<<11)|(3U<<6)|46U, (17U<<21)|(1U<<11)|0x43eU,
        (26U<<21)|(3U<<16)|0x43cU, (21U<<21)|(4U<<16)|0x43dU,
        (18U<<21)|(1U<<11)|0x43fU,
        (30U<<21)|(1U<<11)|(3U<<6)|28U, (30U<<21)|(1U<<11)|(3U<<6)|30U,
        (30U<<21)|(2U<<16)|(1U<<11)|0x03fU, (30U<<21)|(2U<<16)|(1U<<11)|0x07fU,
        (30U<<21)|(2U<<16)|(1U<<11)|0x2bcU, (30U<<21)|(2U<<16)|(1U<<11)|0x2fcU,
        (30U<<21)|(1U<<11)|(3U<<6)|32U, (30U<<21)|(1U<<11)|(3U<<6)|36U,
        (30U<<21)|(1U<<11)|(3U<<6)|34U, (30U<<21)|(1U<<11)|(3U<<6)|38U};
    for (unsigned n=0;n<vector_special.size();++n) {
        code.push_back(nt::r5900::decode(0x460000+n*8,(18U<<26)|vector_special[n]));
        code.push_back(nt::r5900::decode(0x460004+n*8,12));
    }
    // Original constant materialization fixtures, including signed wraparound.
    for (unsigned n=0;const auto word:{0x48c88000U,0x48498000U,0x48c08000U,0x484a8000U,12U})
        code.push_back(nt::r5900::decode(0x460100+n++*4,word));
    for (unsigned group=0;group<3;++group) for (unsigned value=0;value<3;++value) {
        const auto pc=0x470000U+(group*3+value)*16;
        const unsigned high[]{0x8000,0x7fff,0xffff};
        const unsigned low[]{0xffff,0x8000,1};
        const unsigned operation[]{13,9,25}; // ORI, ADDIU, DADDIU.
        code.push_back(nt::r5900::decode(pc,0x3c080000|high[value]));
        code.push_back(nt::r5900::decode(pc+4,(operation[group]<<26)|(8U<<21)|(8U<<16)|low[value]));
        code.push_back(nt::r5900::decode(pc+8,12));
    }
    for (unsigned n=0;const auto word:{0x10000003U,0x3c088000U,0x3508ffffU,12U,12U})
        code.push_back(nt::r5900::decode(0x471000+n++*4,word));
    code.push_back(nt::r5900::decode(0x472000,0x3c088000));
    nt::ee_codegen::CodePayload guarded{0x472004,{0xff,0xff,0x08,0x35,12,0,0,0}};
    code.push_back(nt::r5900::decode(0x472004,0x3508ffff));
    code.push_back(nt::r5900::decode(0x472008,12));
    owned[2]={0x472004,8,1}; images[0].segments.push_back(guarded);
    for (unsigned selector=0;selector<4;++selector) {
        const auto pc=0x480000U+selector*32;
        const auto branch=nt::r5900::decode(pc,0x41000002U|(selector<<16));
        if (!branch.branch() || branch.likely()!=(selector>=2) || branch.target()!=pc+12)
            throw std::runtime_error("Completion branch decoding failed");
        code.push_back(branch);
        code.push_back(nt::r5900::decode(pc+4,0x8c8a0000)); // Potentially faulting delay load.
        code.push_back(nt::r5900::decode(pc+8,12));
        code.push_back(nt::r5900::decode(pc+12,12));
    }
    if (nt::r5900::decode(0x480000,0x41100002).op!=nt::r5900::Op::unsupported)
        throw std::runtime_error("Reserved completion branch encoding was accepted");
    for (unsigned n=0;n<80;++n)
        code.push_back(nt::r5900::decode(0x490000+n*32,12));
    // Original branches whose delay slots straddle stable code-unit boundaries.
    code.push_back(nt::r5900::decode(0x4afffc,0x51000002)); // BEQL t0,zero.
    code.push_back(nt::r5900::decode(0x4b0000,0x25290001));
    code.push_back(nt::r5900::decode(0x4b0004,12));
    code.push_back(nt::r5900::decode(0x4b0008,12));
    code.push_back(nt::r5900::decode(0x4bfffc,0x0c130004)); // JAL 0x4c0010.
    code.push_back(nt::r5900::decode(0x4c0000,0x24020005));
    code.push_back(nt::r5900::decode(0x4c0004,12));
    code.push_back(nt::r5900::decode(0x4c0010,0x03e00008));
    code.push_back(nt::r5900::decode(0x4c0014,0x24420007));
    // Original VSYNC loops with width, alias, likely-branch and unsafe variants.
    for (unsigned variant=0;variant<6;++variant) {
        const auto pc=0x4d0000+variant*32;
        const unsigned load=variant==1 ? 0x8c880010U : variant==2 ? 0x9c880010U : 0xdc880010U;
        const std::array words{load,variant==4 ? 0x31080002U : 0x31080008U,
            variant==2 ? 0x5100fffdU : 0x1100fffdU,variant==3 ? 0x25290001U : 0U,13U};
        for (unsigned n=0;n<words.size();++n)
            code.push_back(nt::r5900::decode(pc+n*4,variant==5 && n==0 ? 0xdd080010U : words[n]));
    }
    // Discover the callback roots before compiling this original counted walk.
    // No callback addresses are supplied as entry points to discovery.
    {
        nt::Bytes image(280,0);
        const auto put=[&](unsigned at,std::uint32_t word) {
            for (unsigned n=0;n<4;++n) image.at(at+n)=std::uint8_t(word>>(n*8));
        };
        nt::elf::Executable executable{0x4e0000,0,{{0,0x4e0000,256,256,5,4},{256,0x2000,24,24,6,4}}};
        const std::uint32_t words[]{0x24100000,0x24112000,0x8e230004,0x26100001,0x26310008,
            0x0060f809,0,0x2e020003,0x1440fff9,0,12};
        for (unsigned n=0;n<std::size(words);++n) put(n*4,words[n]);
        for (unsigned n=0;n<3;++n) {
            put(128+n*16,0x03e00008); put(132+n*16,0x25290001+n);
            put(260+n*8,0x4e0080+n*16);
        }
        auto discovered=nt::analysis::discover(image,executable,256,{},true);
        const auto roots=nt::analysis::initial_indirect_entries(image,executable,discovered);
        if (roots!=std::set<std::uint32_t>{0x4e0080,0x4e0090,0x4e00a0})
            throw std::runtime_error("Counted callback walk failed before LLVM compilation");
        const std::vector<std::uint32_t> entries(roots.begin(),roots.end());
        discovered=nt::analysis::discover(image,executable,256,entries,true);
        for (const auto& [pc,instruction]:discovered.instructions) code.push_back(instruction);
    }
    {
        nt::Bytes image(264,0);
        const auto put=[&](unsigned at,std::uint32_t word) {
            for (unsigned n=0;n<4;++n) image.at(at+n)=std::uint8_t(word>>(n*8));
        };
        nt::elf::Executable executable{0x500000,0,{{0,0x500000,256,256,5,4},{256,0x2000,8,8,6,4}}};
        const std::uint32_t words[]{0x2c820002,0x10400012,0,0x00041880,0x3c020000,
            0x24422000,0x0060282d,0x00621821,0x8c640000,0x00800008,0};
        for (unsigned n=0;n<std::size(words);++n) put(n*4,words[n]);
        put(0x50,12); put(0x60,0x24090007); put(0x64,12); put(0x70,0x24090009); put(0x74,12);
        put(256,0x500060); put(260,0x500070);
        auto discovered=nt::analysis::discover(image,executable,256,{},true);
        const auto roots=nt::analysis::bounded_switch_entries(image,executable,discovered);
        if (roots!=std::set<std::uint32_t>{0x500060,0x500070})
            throw std::runtime_error("Scheduled switch failed before LLVM compilation");
        const std::vector<std::uint32_t> entries(roots.begin(),roots.end());
        discovered=nt::analysis::discover(image,executable,256,entries,true);
        for (const auto& [pc,instruction]:discovered.instructions) code.push_back(instruction);
    }
    code.push_back(nt::r5900::decode(0x4f0000,0x7c860000));
    code.push_back(nt::r5900::decode(0x4f0004,12));
    {
        const std::uint32_t merges[]{0xb0880007,0xb4880000,0x68880007,0x6c880000,
            0xa8880003,0xb8880000,0x88880003,0x98880000};
        for (unsigned n=0;n<std::size(merges);++n) {
            code.push_back(nt::r5900::decode(0x4f0020+n*8,merges[n]));
            code.push_back(nt::r5900::decode(0x4f0024+n*8,12));
        }
    }
    // Original diagnostic programs: live reads, alignment faults and delay slots.
    for (unsigned n=0;n<3;++n) {
        constexpr unsigned selectors[]{13,8,23};
        code.push_back(nt::r5900::decode(0x4f0100+n*8,(16U<<26)|(8U<<16)|(selectors[n]<<11)));
        code.push_back(nt::r5900::decode(0x4f0104+n*8,12));
    }
    for (unsigned n=0;n<2;++n) {
        code.push_back(nt::r5900::decode(0x4f0120+n*8,((n ? 43U : 35U)<<26)|(4U<<21)|(8U<<16)));
        code.push_back(nt::r5900::decode(0x4f0124+n*8,12));
    }
    code.push_back(nt::r5900::decode(0x4f0130,(4U<<26)|1U));
    code.push_back(nt::r5900::decode(0x4f0134,(35U<<26)|(4U<<21)|(8U<<16)));
    code.push_back(nt::r5900::decode(0x4f0138,12));
    code.push_back(nt::r5900::decode(0xfffffffcU,12));
    std::sort(code.begin(),code.end(),[](const auto& left,const auto& right) { return left.pc<right.pc; });
    auto malformed=images; malformed[1].segments[0].bytes[0]^=1;
    failed=false;
    try { std::ostringstream discard; nt::ee_codegen::emit_llvm(discard,code,owned,payloads,layouts,malformed); }
    catch (const std::invalid_argument&) { failed=true; }
    if (!failed) throw std::runtime_error("Mismatched native image bytes accepted");
    malformed=images; malformed[1].id=3; failed=false;
    try { std::ostringstream discard; nt::ee_codegen::emit_llvm(discard,code,owned,payloads,layouts,malformed); }
    catch (const std::invalid_argument&) { failed=true; }
    if (!failed) throw std::runtime_error("Unknown image owner accepted");
    std::ostringstream unavailable; unavailable.setstate(std::ios::badbit);
    failed=false;
    try { nt::ee_codegen::emit_llvm(unavailable,code,owned,payloads,layouts,images); }
    catch (const std::runtime_error&) { failed=true; }
    if (!failed) throw std::runtime_error("Failed LLVM output stream was accepted");
    std::ostringstream generated; nt::ee_codegen::emit_llvm(generated,code,owned,payloads,layouts,images);
    if (generated.str().find("fence seq_cst")==std::string::npos) throw std::runtime_error("Native SYNC ordering was omitted");
    const auto text=generated.str();
    if (text.find("constant_pair4653056:")==std::string::npos)
        throw std::runtime_error("Constant materialization was not grouped");
    if (text.find("constant_pair4661248:")!=std::string::npos)
        throw std::runtime_error("Constant grouping crossed an image ownership boundary");
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
        nt::write_file(std::string(argv[2])+".observation",fixture::flow_observation_elf());
        nt::write_file(std::string(argv[2])+".vector",fixture::vector_container_elf());
        auto object_callbacks=fixture::object_callback_elf();
        nt::write_file(std::string(argv[2])+".copied-callbacks",fixture::copied_callback_elf());
        nt::write_file(std::string(argv[2])+".scaled-callbacks",fixture::scaled_callback_elf());
        nt::write_file(std::string(argv[2])+".long-frame-callbacks",fixture::long_frame_callback_elf());
        nt::write_file(std::string(argv[2])+".object-callbacks",object_callbacks);
        fixture::put32(object_callbacks,0x114,0x24081270);
        fixture::put32(object_callbacks,0x118,0xac880018);
        nt::write_file(std::string(argv[2])+".object-callbacks-mutable",object_callbacks);
        auto mixed_memory=fixture::vector_container_elf();
        const std::uint32_t initialize[]{0x24080005,0xac081060,0x24080007,0xac081080,
            0x8c021060,0x8c081080,0x00481021,0xac001000,0x2403007b,12};
        for (unsigned n=0;n<std::size(initialize);++n) fixture::put32(mixed_memory,0x100+n*4,initialize[n]);
        nt::write_file(std::string(argv[2])+".mixed-memory",mixed_memory);
        nt::Bytes elf(0x184,0);
        const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
        std::copy(std::begin(identity),std::end(identity),elf.begin());
        const auto word=[&](std::size_t at,std::uint32_t value) { fixture::put32(elf,at,value); };
        word(16,0x00080002); word(20,1); word(24,0x1000); word(28,52);
        word(40,0x00200034); word(44,2);
        word(52,1); word(56,0x100); word(60,0x1000); word(68,0x80); word(72,0x80); word(76,5); word(80,4);
        word(84,1); word(88,0x180); word(92,0x2000); word(100,4); word(104,32); word(108,6); word(112,4);
        const std::uint32_t entry[]{0x24042000,0x8c880000,0x8c890004,0x25080001,0xac880004,0x24051040,0x0c000418,0,0x8c990008,0x0320f809,0x25290001,0x0000000c};
        for (unsigned i=0;i<std::size(entry);++i) word(0x100+i*4,entry[i]);
        word(0x130,0xffffffff); // Unreachable after the syscall: must not be translated.
        word(0x140,0x8c820004); word(0x144,0x03e00008); word(0x148,0x00491021); word(0x180,41);
        word(0x160,0xac850008); word(0x164,0x03e00008); word(0x168,0);
        nt::write_file(argv[2],elf);
        const auto original=elf;
        elf.resize(0x204,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x90); word(72,0x90); word(88,0x200); word(100,4); word(104,4);
        const std::uint32_t conditional[]{0x24021080,0x24042000,0x14000003,0,
            0x0c000410,0xac820000,0x8c990000,0x0320f809,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(conditional);++n) word(0x100+n*4,conditional[n]);
        word(0x140,0x03e00008); word(0x180,0x03e00008); word(0x184,0x24020049);
        nt::write_file(std::string(argv[2])+".conditional-callback",elf);
        std::fill(elf.begin()+0x100,elf.end(),0);
        word(0x100,0x0c000410); word(0x104,0x24051080); word(0x108,0x2403007b); word(0x10c,12);
        const std::uint32_t visitor[]{0x03e0802d,0x00a0882d,0x0c000418,0,0x0220f809,0,0x02000008,0};
        for (unsigned n=0;n<std::size(visitor);++n) word(0x140+n*4,visitor[n]);
        word(0x160,0x03e00008); word(0x180,0x03e00008); word(0x184,0x2402005b);
        nt::write_file(std::string(argv[2])+".invoked-callback",elf);
        // A wrapper forwards the callback in a delay slot to a second helper.
        // Only the helper stores it; the caller later invokes the actual value.
        elf.resize(0x224,0); word(68,0xe0); word(72,0xe0); word(88,0x220);
        std::fill(elf.begin()+0x100,elf.end(),0);
        word(0x100,0x0c000410); word(0x104,0x24051080); word(0x108,0x2403007b); word(0x10c,12);
        const std::uint32_t forwarding[]{0x03e0802d,0x0c000428,0x00a0302d,
            0x24042000,0x8c990000,0x0320f809,0,0x02000008,0};
        for (unsigned n=0;n<std::size(forwarding);++n) word(0x140+n*4,forwarding[n]);
        word(0x180,0x03e00008); word(0x184,0x24020061);
        word(0x1a0,0x24042000); word(0x1a4,0xac860000); word(0x1a8,0x03e00008);
        nt::write_file(std::string(argv[2])+".forwarded-callback",elf);
        // Two local function pointers pass through a tail jump and are invoked
        // from a stack record. Their results prove both targets were compiled.
        std::fill(elf.begin()+0x100,elf.end(),0);
        word(104,32);
        word(0x100,0x241d2000); word(0x104,0x0c000410); word(0x10c,0x2403007b); word(0x110,12);
        const std::uint32_t tail[]{0x240510c0,0x08000420,0x240610d0};
        for (unsigned n=0;n<std::size(tail);++n) word(0x140+n*4,tail[n]);
        const std::uint32_t invoke[]{0x03e0802d,0xafa50000,0xafa60004,0x8fb90000,0x0320f809,0,
            0x0040882d,0x8fb90004,0x0320f809,0,0x00511021,0x02000008,0};
        for (unsigned n=0;n<std::size(invoke);++n) word(0x180+n*4,invoke[n]);
        word(0x1c0,0x03e00008); word(0x1c4,0x24020017);
        word(0x1d0,0x03e00008); word(0x1d4,0x2402001f);
        nt::write_file(std::string(argv[2])+".tail-callbacks",elf);
        elf.resize(0x304,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x1c0); word(72,0x1c0); word(88,0x300);
        const std::uint32_t forms[]{0x241d2000,0x24051180,0x0c000430,0,0x00409021,
            0x24051190,0x241910c0,0x0320f809,0,0x02429021,
            0x240511a0,0x04130024,0,0x02429021,0x0c000440,0,
            0x00402821,0x0c000430,0,0x02421021,0x2403007b,12};
        for (unsigned n=0;n<std::size(forms);++n) word(0x100+n*4,forms[n]);
        const std::uint32_t consume[]{0x03e08021,0x00a0c821,0x44800000,0x46000000,0x0320f809,0,0x02000008,0};
        for (unsigned n=0;n<std::size(consume);++n) word(0x1c0+n*4,consume[n]);
        word(0x200,0x03e00008); word(0x204,0x240211b0);
        for (const auto [at,value]:std::array<std::pair<unsigned,unsigned>,4>{{
                {0x280,23},{0x290,31},{0x2a0,47},{0x2b0,113}}}) {
            word(at,0x03e00008); word(at+4,0x24020000|value);
        }
        nt::write_file(std::string(argv[2])+".call-forms",elf);
        // The constructor stores both a payload and a callback. Only the
        // callback is executable; the payload deliberately starts with an
        // unsupported word. A second callback arrives as a stack argument.
        elf=original;
        elf.resize(0x284,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x170); word(72,0x170); word(88,0x280); word(100,4); word(104,64);
        const std::uint32_t mixed_main[]{0x241d2000,0x24042020,0x24051140,0x24061150,0x0c000428,0,
            0x8c48000c,0x91100000,0x8c590010,0x0320f809,0,0x00408821,
            0x24081160,0xafa80000,0x0c000430,0,0x02221021,0x02021021,0x2403007b,12};
        for (unsigned n=0;n<std::size(mixed_main);++n) word(0x100+n*4,mixed_main[n]);
        word(0x1a0,0xac85000c); word(0x1a4,0xac860010); word(0x1a8,0x03e00008); word(0x1ac,0x00801021);
        const std::uint32_t stack_callback[]{0x03e09021,0x8fb90000,0x0320f809,0,0x02400008,0};
        for (unsigned n=0;n<std::size(stack_callback);++n) word(0x1c0+n*4,stack_callback[n]);
        word(0x240,0x4c0000ff); word(0x250,0x03e00008); word(0x254,0x24020025);
        word(0x260,0x03e00008); word(0x264,0x24020005);
        nt::write_file(std::string(argv[2])+".pointer-uses",elf);
        // Moving the same unsupported word to an actual callback must reject
        // conversion; classification must never be based on opcode support.
        word(0x250,0x4c0000ff);
        nt::write_file(std::string(argv[2])+".unsupported-callback",elf);
        elf=original;
        // Cross the code-module boundary using original synthetic straight-line
        // code. Both the final native function and its preceding module execute.
        constexpr unsigned module_words=32*nt::ee_codegen::function_instruction_limit+3;
        elf.resize(0x100+module_words*4,0);
        std::fill(elf.begin()+0x100,elf.end(),0);
        elf[44]=1; elf[45]=0;
        word(68,module_words*4); word(72,module_words*4);
        word(0x100,0x24020009);
        word(0x100+(module_words-2)*4,0x2403007b);
        word(0x100+(module_words-1)*4,12);
        nt::write_file(std::string(argv[2])+".modules",elf);
        elf=original;
        elf.resize(0x258,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x140); word(72,0x140); word(88,0x240); word(100,24); word(104,24);
        const std::uint32_t descriptor_main[]{0x24042000,0x0c000410,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(descriptor_main);++n) word(0x100+n*4,descriptor_main[n]);
        const std::uint32_t consumer[]{0x0080802d,0x03e0882d,0x24120000,
            0x8e190000,0x0320f809,0,0x02429021,
            0x8e190008,0x0320f809,0,0x02429021,
            0x8e190010,0x0320f809,0,0x02429021,0x0240102d,0x02200008,0};
        for (unsigned n=0;n<std::size(consumer);++n) word(0x140+n*4,consumer[n]);
        for (unsigned n=0;n<3;++n) {
            word(0x200+n*16,0x03e00008); word(0x204+n*16,0x24020001+n);
            word(0x240+n*8,0x1100+n*16);
        }
        nt::write_file(std::string(argv[2])+".descriptors",elf);
        elf=original;
        elf.resize(0x188,0); word(100,8);
        const std::uint32_t indirect[]{0x24100002,0x24032000,0x84640000,0x8c650004,0x00a0f809,
            0x02042021,0x2403007b,12};
        for (unsigned i=0;i<std::size(indirect);++i) word(0x100+i*4,indirect[i]);
        word(0x140,0x0080102d); word(0x144,0x03e00008); word(0x148,0);
        word(0x180,7); word(0x184,0x1040);
        nt::write_file(std::string(argv[2])+".indirect",elf);
        // Runtime mutates a callback slot between calls. Both targets must be
        // discovered, but neither call may be folded to an initial slot value.
        elf=original;
        elf.resize(0x208,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x100); word(72,0x100); word(88,0x200); word(100,8); word(104,16);
        const std::uint32_t pointer_flow[]{0x24042000,0x24081080,0xac880000,0x8c990000,
            0x10000003,0,0,0,0x0320f809,0,
            0x24081090,0xac880000,0x8c990000,0x0320f809,0,
            0x240810a0,0xac880004,0x24193000,0x0320f809,0};
        for (unsigned n=0;n<std::size(pointer_flow);++n) word(0x100+n*4,pointer_flow[n]);
        word(0x180,0x03e00008); word(0x184,0x2402000b);
        word(0x190,0x03e00008); word(0x194,0x24020016);
        word(0x1a0,0x4c000000); // Uncalled payload must never be compiled.
        nt::write_file(std::string(argv[2])+".pointer-flow",elf);
        word(0x110,0x08000428); // Direct flow now reaches the unsupported payload.
        nt::write_file(std::string(argv[2])+".pointer-flow-unsupported",elf);
        elf=original;
        elf.resize(0x208,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x100); word(72,0x100); word(88,0x200); word(100,8); word(104,256);
        const std::uint32_t table_setup[]{0x24092000,0x24082040,0xad090000,0x24070002,0xad070004,
            0x240610e0,0x10000002,0,0,0xad26000c,0x240a10d0,0xad2a0010,
            0x24050001,0x0c000420,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(table_setup);++n) word(0x100+n*4,table_setup[n]);
        const std::uint32_t table_consumer[]{0x24112040,0x8e240004,0x00a4382b,0x10e00009,
            0x2402000c,0x8e230000,0x00a22818,0x00a31021,0x8c460000,0x00c00008,0};
        for (unsigned n=0;n<std::size(table_consumer);++n) word(0x180+n*4,table_consumer[n]);
        word(0x1b4,0x03e00008); word(0x1d0,0x4c000000);
        word(0x1e0,0x03e00008); word(0x1e4,0x2402002a);
        nt::write_file(std::string(argv[2])+".indexed-callback",elf);
        // The same table is populated by a helper with a runtime record index.
        // Callback and opaque context are both executable-looking arguments.
        elf.resize(0x308,0);
        word(68,0x180); word(72,0x180); word(88,0x300);
        std::fill(elf.begin()+0x100,elf.begin()+0x180,0);
        const std::uint32_t registration_setup[]{0x24092000,0x24082040,0xad090000,0x24070002,0xad070004,
            0x24040001,0x240510e0,0x240610d0,0x0c000440,0,
            0x24050001,0x0c000420,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(registration_setup);++n) word(0x100+n*4,registration_setup[n]);
        const std::uint32_t registration[]{0x24082040,0x8d090000,0x240a000c,0x008a5018,
            0x012a4821,0xad250000,0xad260004,0x03e00008,0};
        for (unsigned n=0;n<std::size(registration);++n) word(0x200+n*4,registration[n]);
        nt::write_file(std::string(argv[2])+".registered-indexed-callback",elf);
        std::fill(elf.begin()+0x100,elf.end(),0);
        const std::uint32_t nested_setup[]{0x24042000,0x240510e0,0x240610d0,0x24072040,
            0x0c000440,0,0x24042040,0x0c000420,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(nested_setup);++n) word(0x100+n*4,nested_setup[n]);
        const std::uint32_t nested_consumer[]{0x8c88001c,0x8d19001c,0x03200008,0};
        for (unsigned n=0;n<std::size(nested_consumer);++n) word(0x180+n*4,nested_consumer[n]);
        const std::uint32_t nested_registration[]{0xac85001c,0xac860020,0xace4001c,0x03e00008,0};
        for (unsigned n=0;n<std::size(nested_registration);++n) word(0x200+n*4,nested_registration[n]);
        word(0x1d0,0x4c000000); word(0x1e0,0x03e00008); word(0x1e4,0x2402002a);
        nt::write_file(std::string(argv[2])+".nested-callback",elf);
        word(0x200,0xac850018); // Different field: no callback-consumer evidence.
        nt::write_file(std::string(argv[2])+".nested-callback-unrelated",elf);
        std::fill(elf.begin()+0x100,elf.end(),0);
        const std::uint32_t referenced_setup[]{0x241d2040,0x24042000,0x03a0282d,0x240610d0,
            0x240810e0,0x10800003,0,0x0c000440,0xafa80000,0x24042000,0x0c000420,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(referenced_setup);++n) word(0x100+n*4,referenced_setup[n]);
        const std::uint32_t bound_consumer[]{0x8c990008,0x03200008,0x8c84000c};
        for (unsigned n=0;n<std::size(bound_consumer);++n) word(0x180+n*4,bound_consumer[n]);
        const std::uint32_t reference_setter[]{0x8ca80000,0xac86000c,0x03e00008,0xac880008};
        for (unsigned n=0;n<std::size(reference_setter);++n) word(0x200+n*4,reference_setter[n]);
        word(0x1d0,0x4c000000); word(0x1e0,0x03e00008); word(0x1e4,0x2402002a);
        nt::write_file(std::string(argv[2])+".referenced-stack-callback",elf);
        word(0x204,0xac860010); // Not the context field supplied at the indirect call.
        nt::write_file(std::string(argv[2])+".referenced-stack-unrelated",elf);
        // Copy a function/context record by reference after an intervening call.
        // The consumer addresses the embedded record through a displaced base.
        elf.resize(0x388,0); word(68,0x280); word(72,0x280); word(88,0x380);
        std::fill(elf.begin()+0x100,elf.end(),0);
        const std::uint32_t record_main[]{0x241d2080,0x0c000440,0,0x2403007b,12,0x03e00008,0};
        for (unsigned n=0;n<std::size(record_main);++n) word(0x100+n*4,record_main[n]);
        const std::uint32_t record_consumer[]{0x24880018,0x8d190000,0x03200008,0x8d040004};
        for (unsigned n=0;n<std::size(record_consumer);++n) word(0x180+n*4,record_consumer[n]);
        const std::uint32_t record_caller[]{0x03e08021,0x27bdfff0,0x03a08821,0x240810e0,
            0xafa80000,0x240810d0,0xafa80004,0x0c000460,0,0x24042000,0x02202821,
            0x0c000450,0,0x24042000,0x0c000420,0,0x27bd0010,0x02000008,0};
        // Leave room between the caller and its leaf setter.
        for (unsigned n=0;n<std::size(record_caller);++n) word(0x200+n*4,record_caller[n]);
        word(0x22c,0x0c000468); // setter at 0x11a0
        word(0x230,0x24060000);
        // Address arithmetic loses argument identity, as with an allocated object.
        const std::uint32_t record_setter[]{0x00862021,0x68a80007,0x6ca80000,0xb088001f,0xb4880018,0x03e00008,0};
        for (unsigned n=0;n<std::size(record_setter);++n) word(0x2a0+n*4,record_setter[n]);
        word(0x280,0x03e00008); word(0x284,0);
        word(0x1d0,0x4c000000); word(0x1e0,0x03e00008); word(0x1e4,0x2402002a);
        nt::write_file(std::string(argv[2])+".copied-callback-record",elf);
        word(0x2ac,0xb088001b); word(0x2b0,0xb4880014);
        nt::write_file(std::string(argv[2])+".copied-record-unrelated",elf);
        word(0x2a4,0xdca80000); word(0x2a8,0xfc880018); word(0x2ac,0x03e00008); word(0x2b0,0);
        nt::write_file(std::string(argv[2])+".copied-aligned-record",elf);
        // The callback reaches the stack before a callee clobbers v0. A later
        // wrapper must recover that stored value, not the callee's return value.
        std::fill(elf.begin()+0x200,elf.begin()+0x280,0);
        const std::uint32_t call_slot_record[]{0x03e08021,0x27bdfff0,0x03a08821,0x240210e0,
            0x0c000460,0xafa20000,0x240810d0,0xafa80004,0x0c000460,0,
            0x24042000,0x02202821,0x0c000454,0x24060000,0x24042000,0x0c000420,0,
            0x27bd0010,0x02000008,0};
        for (unsigned n=0;n<std::size(call_slot_record);++n) word(0x200+n*4,call_slot_record[n]);
        const std::uint32_t record_wrapper[]{0x27bdfff0,0xffbf0008,0x0c000468,0,
            0xdfbf0008,0x03e00008,0x27bd0010};
        for (unsigned n=0;n<std::size(record_wrapper);++n) word(0x250+n*4,record_wrapper[n]);
        word(0x284,0x24020000);
        nt::write_file(std::string(argv[2])+".call-slot-record",elf);
        word(0x224,0xafa00000);
        nt::write_file(std::string(argv[2])+".call-slot-overwritten",elf);
        // Compilers also inline the same record copy; the source can use a
        // preserved stack alias established before its fields are written.
        std::fill(elf.begin()+0x200,elf.begin()+0x280,0);
        const std::uint32_t inline_record[]{0x03e08021,0x27bdfff0,0x03a08821,0x240810e0,
            0xafa80000,0x240810d0,0xafa80004,0x24042000,0x6a280007,0x6e280000,
            0xb088001f,0xb4880018,0x0c000420,0,0x27bd0010,0x02000008,0};
        for (unsigned n=0;n<std::size(inline_record);++n) word(0x200+n*4,inline_record[n]);
        nt::write_file(std::string(argv[2])+".inline-callback-record",elf);
        word(0x218,0xb3a00007);
        nt::write_file(std::string(argv[2])+".inline-record-overwritten",elf);
        word(0x218,0xaba00003);
        nt::write_file(std::string(argv[2])+".inline-record-word-overwritten",elf);
        word(0x218,0xafa80004);
        word(0x22c,0xb4880014);
        nt::write_file(std::string(argv[2])+".inline-record-partial",elf);
        word(0x22c,0xb4880018); word(0x224,0x6e280004);
        nt::write_file(std::string(argv[2])+".inline-record-source",elf);
        word(0x220,0xde280000); word(0x224,0xfc880018); word(0x228,0); word(0x22c,0);
        nt::write_file(std::string(argv[2])+".inline-aligned-record",elf);
        word(0x224,0xfc880014);
        nt::write_file(std::string(argv[2])+".inline-record-unrelated",elf);
        // Initialize a record in a factory, return it through a wrapper, then
        // save/reload its pointer and replace the callback before invoking it.
        std::fill(elf.begin()+0x200,elf.begin()+0x2c0,0);
        const std::uint32_t returned_record[]{0x03e08021,0x27bdfff0,0xafa00000,0xafa00004,
            0x24042000,0x03a02821,0x24060000,0x0c000458,0,0xafa20008,
            0x240810e0,0x240910d0,0xac49001c,0x8fa30008,0xac680018,0x00602021,
            0x0c000420,0,0x27bd0010,0x02000008,0};
        for (unsigned n=0;n<std::size(returned_record);++n) word(0x200+n*4,returned_record[n]);
        for (unsigned n=0;n<std::size(record_wrapper);++n) word(0x260+n*4,record_wrapper[n]);
        const std::uint32_t record_factory[]{0x00861021,0xdca80000,0xfc480018,0x03e00008,0};
        for (unsigned n=0;n<std::size(record_factory);++n) word(0x2a0+n*4,record_factory[n]);
        nt::write_file(std::string(argv[2])+".returned-callback-record",elf);
        // Verification inventories gaps without promoting matching data layouts.
        std::fill(elf.begin()+0x100,elf.end(),0);
        word(0x100,0x2403007b); word(0x104,12);
        nt::write_file(std::string(argv[2])+".flow-direct",elf);
        const std::uint32_t flow_main[]{0x24042000,0x0c000440,0,0x24042040,
            0x0c000420,0,0x2403007b,12};
        for (unsigned n=0;n<std::size(flow_main);++n) word(0x100+n*4,flow_main[n]);
        const std::uint32_t flow_consumer[]{0x8c990018,0x03200008,0x8c84001c};
        const std::uint32_t flow_initializer[]{0x240810e0,0xac880018,0x240910d0,0xac89001c,0x03e00008,0};
        for (unsigned n=0;n<std::size(flow_consumer);++n) word(0x180+n*4,flow_consumer[n]);
        for (unsigned n=0;n<std::size(flow_initializer);++n) word(0x200+n*4,flow_initializer[n]);
        word(0x1d0,0x4c000000); word(0x1e0,0x03e00008); word(0x1e4,0x2402002a);
        nt::write_file(std::string(argv[2])+".flow-unlinked",elf);
        // A thread descriptor remains in s4 across a call and a retry edge.
        // Its entry is discovered only from the descriptor passed to CreateThread.
        elf=original;
        elf.resize(0x308,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x200); word(72,0x200); word(88,0x300); word(100,8); word(104,0x6000);
        const std::uint32_t thread_main[]{0x24040000,0x24056000,0x24061000,0x24072000,
            0x240811e0,0x2403003c,12,0x0040e821,0x0c000420,0,
            0x00402021,0x2405002a,0x24030022,12,0x24030032,12,0x2403007b,12};
        for (unsigned n=0;n<std::size(thread_main);++n) word(0x100+n*4,thread_main[n]);
        word(0x148,0x03e00008);
        const std::uint32_t thread_creator[]{0x27bdffc0,0xafbf003c,0x27b40010,0x240811c0,
            0xafa80014,0x24084000,0xafa80018,0x24081000,0xafa8001c,0x2408000a,0xafa80024,
            0,0x0c000460,0,0x10000002,0,0,0x0c000450,0x0280202d,
            0x0440fff8,0,0x8fbf003c,0x03e00008,0x27bd0040};
        for (unsigned n=0;n<std::size(thread_creator);++n) word(0x180+n*4,thread_creator[n]);
        word(0x240,0x24030020); word(0x244,12); word(0x248,0x03e00008); word(0x24c,0);
        word(0x280,0x03e00008); word(0x284,0x24080000);
        word(0x2c0,0x00801021); word(0x2c4,0x2403007b); word(0x2c8,12);
        word(0x2cc,0x03e00008);
        word(0x2e0,0x24030023); word(0x2e4,12); word(0x2e8,0x03e00008);
        nt::write_file(std::string(argv[2])+".frame-thread",elf);
        word(0x1ac,0xa2800004); // Aliased partial overwrite invalidates the older entry.
        nt::write_file(std::string(argv[2])+".frame-thread-overwritten",elf);
        word(0x1ac,0xae800004);
        nt::write_file(std::string(argv[2])+".frame-thread-cleared",elf);
        elf=original;
        elf.resize(0x308,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x200); word(72,0x200); word(88,0x300); word(100,8); word(104,0x100);
        const std::uint32_t loop_setup[]{0x24040000,0x240510e0,0x240610d0,0x0c000450,0,
            0x24040001,0x0c000450,0,0x0c000420,0,0x2403007b,12,0x03e00008,0};
        for (unsigned n=0;n<std::size(loop_setup);++n) word(0x100+n*4,loop_setup[n]);
        const std::uint32_t loop_consumer[]{0x24102000,0x24110002,0x24020000,0x03e09021,
            0x8e190000,0x53200004,0x2631ffff,0x0320f809,0x8e040004,0x2631ffff,
            0x0621fff9,0x26100008,0x02400008,0};
        for (unsigned n=0;n<std::size(loop_consumer);++n) word(0x180+n*4,loop_consumer[n]);
        const std::uint32_t loop_setter[]{0x000440c0,0x24092000,0x01284821,0xad260004,
            0x03e00008,0xad250000};
        for (unsigned n=0;n<std::size(loop_setter);++n) word(0x240+n*4,loop_setter[n]);
        word(0x1d0,0x4c000000); word(0x1e0,0x24420015); word(0x1e4,0x03e00008); word(0x1e8,0);
        nt::write_file(std::string(argv[2])+".countdown-callbacks",elf);
        word(0x198,0x2631fffe); // One arm changes the count differently: no proved bound.
        nt::write_file(std::string(argv[2])+".countdown-unproved",elf);
        // A constructor registers a callback stored in an object. A forwarding
        // helper copies that field and its owning object into an indexed slot.
        // The consumer loads the context before loading/calling the function.
        elf=original;
        elf.resize(0x608,0); std::fill(elf.begin()+0x100,elf.end(),0);
        word(68,0x500); word(72,0x500); word(88,0x600); word(100,8); word(104,0x400);
        const std::uint32_t object_main[]{0x241d2380,0x24042000,0x24052100,0x0c000440,0,
            0x24042100,0x24050001,0x0c000460,0,0x2403007b,12,0x03e00008,0};
        for (unsigned n=0;n<std::size(object_main);++n) word(0x100+n*4,object_main[n]);
        const std::uint32_t object_constructor[]{0x27bdfff0,0xffbf0008,0x00808021,
            0x24081200,0xae080014,0x24081210,0xae080018,0x24080001,0xae08000c,
            0x2408002a,0xae08001c,0,0x00a02021,0x0c000458,0x02002821,
            0xdfbf0008,0x03e00008,0x27bd0010};
        for (unsigned n=0;n<std::size(object_constructor);++n) word(0x200+n*4,object_constructor[n]);
        const std::uint32_t object_forwarder[]{0x00a03821,0,0x8ca60014,0x8ca8000c,
            0x000840c0,0x00882821,0x08000470,0x24a50020};
        for (unsigned n=0;n<std::size(object_forwarder);++n) word(0x260+n*4,object_forwarder[n]);
        const std::uint32_t object_consumer[]{0x000540c0,0x00884021,0x25080020,
            0x8d040004,0x8d190000,0x03200008,0};
        for (unsigned n=0;n<std::size(object_consumer);++n) word(0x280+n*4,object_consumer[n]);
        const std::uint32_t object_setter[]{0xaca70004,0x03e00008,0xaca60000};
        for (unsigned n=0;n<std::size(object_setter);++n) word(0x2c0+n*4,object_setter[n]);
        word(0x300,0x03e00008); word(0x304,0x8c82001c); word(0x310,0x4c000000);
        nt::write_file(std::string(argv[2])+".object-field-registration",elf);
        word(0x22c,0xa2000015);
        nt::write_file(std::string(argv[2])+".object-field-overwritten",elf);
        word(0x22c,0xae000014);
        nt::write_file(std::string(argv[2])+".object-field-cleared",elf);
        word(0x228,0x02008821); word(0x22c,0xa2200015);
        nt::write_file(std::string(argv[2])+".object-field-alias-overwritten",elf);
        word(0x228,0xae08001c);
        word(0x22c,0); word(0x2c0,0xaca70008);
        nt::write_file(std::string(argv[2])+".object-field-unrelated",elf);
        word(0x2c0,0xaca70004); word(0x264,0xaca00014);
        nt::write_file(std::string(argv[2])+".object-field-callee-cleared",elf);
        word(0x264,0); word(0x260,0x00803821);
        nt::write_file(std::string(argv[2])+".object-field-unrelated-context",elf);
        word(0x260,0x00a03821); word(0x264,0x8ca8000c); word(0x26c,0xaca00014);
        nt::write_file(std::string(argv[2])+".object-field-loaded-before-clear",elf);
        // The same registry also accepts a callback argument directly, and its
        // consumer checks for null before loading the adjacent context.
        word(0x22c,0x24061200); word(0x264,0); word(0x268,0); word(0x26c,0x8ca8000c);
        const std::uint32_t guarded_consumer[]{0x000540c0,0x00884021,0x25080020,
            0x8d190000,0x53200007,0,0x8d040004,0x03200008,0};
        for (unsigned n=0;n<std::size(guarded_consumer);++n) word(0x280+n*4,guarded_consumer[n]);
        word(0x2b0,0x03e00008); word(0x2b4,0);
        nt::write_file(std::string(argv[2])+".object-argument-registration",elf);
        word(0x2c0,0xaca70008);
        nt::write_file(std::string(argv[2])+".object-argument-unrelated",elf);
        // A handle-selected callback record saves the registering GP alongside
        // the function and user data. Invocation restores GP and passes the
        // user data as a later argument, not necessarily a0.
        std::fill(elf.begin()+0x100,elf.end(),0);
        const std::uint32_t gp_main[]{0x241d2380,0x241c0006,0x24042003,0x24051210,
            0x24061200,0x24072040,0x2408002a,0xace80000,0x0c000440,0,
            0x241c0009,0x24042000,0x0c000460,0,0x2403007b,12,0x03e00008,0};
        for (unsigned n=0;n<std::size(gp_main);++n) word(0x100+n*4,gp_main[n]);
        const std::uint32_t gp_setter[]{0x3084fff0,0xac850020,0x10c00003,0xac860010,
            0xac9c0014,0xac870018,0x03e00008,0};
        for (unsigned n=0;n<std::size(gp_setter);++n) word(0x200+n*4,gp_setter[n]);
        const std::uint32_t gp_consumer[]{0x03e08021,0x8c9c0014,0x8c990010,
            0x0320f809,0x8c870018,0x02000008,0};
        for (unsigned n=0;n<std::size(gp_consumer);++n) word(0x280+n*4,gp_consumer[n]);
        word(0x300,0x8ce20000); word(0x304,0x005c1021); word(0x308,0x03e00008);
        word(0x310,0x4c000000);
        nt::write_file(std::string(argv[2])+".gp-callback-record",elf);
        word(0x210,0xac9c0024);
        nt::write_file(std::string(argv[2])+".gp-record-unrelated",elf);
        word(0x210,0xac9c0014); word(0x214,0xac87001c);
        nt::write_file(std::string(argv[2])+".gp-record-context",elf);
        word(0x214,0xa0800011); word(0x218,0xac870018); word(0x21c,0x03e00008); word(0x220,0);
        nt::write_file(std::string(argv[2])+".gp-record-overwritten",elf);
        word(0x210,0x24840040); word(0x214,0xac9c0014);
        nt::write_file(std::string(argv[2])+".gp-record-base-reused",elf);
        // No path stores both the function and GP: merging branch summaries
        // must not manufacture a callback registration.
        const std::uint32_t split_record[]{0x3084fff0,0x10a00004,0,
            0xac860010,0x10000002,0,0xac9c0014,0xac870018,0x03e00008,0};
        for (unsigned n=0;n<std::size(split_record);++n) word(0x200+n*4,split_record[n]);
        nt::write_file(std::string(argv[2])+".gp-record-disjoint",elf);
        // The registered function can itself invoke a callback in its opaque
        // context. Follow that known function's field use back to registration.
        std::fill(elf.begin()+0x100,elf.end(),0);
        const std::uint32_t closure_main[]{0x241d2380,0x241c0006,0x24042083,0x24051230,
            0x24062040,0x24071210,0x2408002a,0xacc80000,0x0c000420,0,
            0x241c0009,0x24042000,0x0c000460,0,0x2403007b,12,0x03e00008,0};
        for (unsigned n=0;n<std::size(closure_main);++n) word(0x100+n*4,closure_main[n]);
        const std::uint32_t closure_setup[]{0x27bdfff0,0xffbf0008,0x3084fff0,
            0x04830003,0xac850008,0x10000009,0,0xac86000c,0xac870010,
            0x00803821,0x24042003,0x24051210,0x24061200,0x0c000440,0,
            0xdfbf0008,0x03e00008,0x27bd0010};
        for (unsigned n=0;n<std::size(closure_setup);++n) word(0x180+n*4,closure_setup[n]);
        for (unsigned n=0;n<std::size(gp_setter);++n) word(0x200+n*4,gp_setter[n]);
        for (unsigned n=0;n<std::size(gp_consumer);++n) word(0x280+n*4,gp_consumer[n]);
        word(0x300,0x8ce20008); word(0x304,0x00400008); word(0x308,0x8ce7000c);
        word(0x310,0x4c000000);
        word(0x330,0x8ce20000); word(0x334,0x005c1021); word(0x338,0x03e00008);
        nt::write_file(std::string(argv[2])+".registered-context-callback",elf);
        word(0x1a4,0x240720c0);
        nt::write_file(std::string(argv[2])+".registered-context-unrelated",elf);
        word(0x1a4,0x00803821); word(0x190,0xac850014);
        nt::write_file(std::string(argv[2])+".registered-context-field",elf);
        word(0x190,0xac850008); word(0x1a0,0xa0800009);
        nt::write_file(std::string(argv[2])+".registered-context-overwritten",elf);
        word(0x1a0,0xac870010); word(0x18c,0x54000003);
        nt::write_file(std::string(argv[2])+".registered-context-annulled",elf);
        elf=original;
        elf.resize(0x190,0); word(100,16); word(104,36);
        const std::uint32_t constructors[]{0x24102000,0x8e020000,0x8e190004,0x0320f809,0,
            0x8e190008,0x0320f809,0,0x8e020020,0x2403007b,12};
        for (unsigned i=0;i<std::size(constructors);++i) word(0x100+i*4,constructors[i]);
        for (unsigned n=0;n<2;++n) {
            const auto at=0x140+n*0x20;
            word(at,0x8e020020); word(at+4,0x24420003+n*2); word(at+8,0xae020020);
            word(at+12,0x03e00008); word(at+16,0);
        }
        word(0x180,0xffffffff); word(0x184,0x1040); word(0x188,0x1060); word(0x18c,0);
        nt::write_file(std::string(argv[2])+".constructors",elf);
        elf=original;
        // Native loader stores an original synthetic image and hands off via ExecPS2.
        const std::uint32_t loader[]{0x24040000,0x34058000,0x24061000,0x24072000,0x24081000,
            0x2403003c,12,0x0040e821,
            0x3c082402,0x35080056,0xac083000,
            0x3c082403,0x3508007b,0xac083004,0x2408000c,0xac083008,
            0x24043000,0x24050000,0x24060000,0x24070000,0x24030007,12,0x03e00008,0};
        for (unsigned i=0;i<std::size(loader);++i) word(0x100+i*4,loader[i]);
        nt::write_file(std::string(argv[2])+".loader",elf);
        elf=original; word(24,0x3000); word(44,1); word(60,0x3000); word(68,12); word(72,12);
        word(0x100,0x24020056); word(0x104,0x2403007b); word(0x108,12);
        nt::write_file(std::string(argv[2])+".image",elf);
        elf=original;
        elf.resize(0x280,0); word(100,0x100); word(104,0x100);
        const std::uint32_t table_call[]{0x24042040,0x24082000,0xac880044,0x8c820044,
            0x24420018,0x8c450000,0x8c430004,0x0060f809,0x00852021,12};
        for (unsigned i=0;i<std::size(table_call);++i) word(0x100+i*4,table_call[i]);
        word(0x160,0x2402002b); word(0x164,0x03e00008); word(0x168,0);
        word(0x198,4); word(0x19c,0x1060);
        nt::write_file(std::string(argv[2])+".table",elf);
        elf=original;
        elf.resize(0x188,0); word(100,8);
        const std::uint32_t switch_call[]{0x24040001,0x2c820002,0x10400009,0x00a03021,
            0x3c030000,0x00041080,0x24632000,0x00621821,0x8c630000,0x00600008,0};
        for (unsigned i=0;i<std::size(switch_call);++i) word(0x100+i*4,switch_call[i]);
        word(0x130,12); word(0x140,0x2402002b); word(0x144,12);
        word(0x150,0x2402002c); word(0x154,12); word(0x180,0x1040); word(0x184,0x1050);
        nt::write_file(std::string(argv[2])+".switch",elf);
        elf=original;
        const std::uint32_t startup[]{0x24040000,0x24054000,0x24061000,0x24072000,0x24081040,
            0x2403003c,12,0x0040e821,0x24043000,0x2405ffff,0x2403003d,12,
            0x00408021,0x2403003e,12,0x2403007b,12,0x03e00008,0};
        for (unsigned i=0;i<std::size(startup);++i) word(0x100+i*4,startup[i]);
        nt::write_file(std::string(argv[2])+".startup",elf);
        word(0x110,0x24081070); word(0x13c,0x01000008); word(0x140,0);
        word(0x170,0x2402004d); word(0x174,0x2403007b); word(0x178,12);
        nt::write_file(std::string(argv[2])+".thread-root",elf);
        elf=original;
        elf.resize(0x300,0);
        word(100,32); word(32,0x200); word(44,0x00280002); word(48,0x00020003);
        word(0x22c,1); word(0x228,1); word(0x230,2); word(0x234,0x2010);
        word(0x238,0x190); word(0x23c,12); word(0x248,4);
        word(0x254,3); word(0x260,0x280); word(0x264,8);
        const std::string section_name("\0.ctors\0",8);
        std::copy(section_name.begin(),section_name.end(),elf.begin()+0x280);
        word(0x190,0xffffffff); word(0x194,0x1070); word(0x198,0);
        word(0x12c,0x8c990014); word(0x130,0x0320f809); word(0x134,0); word(0x138,12);
        word(0x170,0x24420007); word(0x174,0x03e00008); word(0x178,0);
        nt::write_file(std::string(argv[2])+".lifecycle",elf);
        elf=original;
        elf.resize(0x400,0);
        word(68,0x140); word(72,0x140); word(88,0x300); word(100,0x100); word(104,0x100);
        const std::uint32_t sleep_main[]{0x24040000,0x34058000,0x24061000,0x24072000,0x24081120,
            0x2403003c,12,0x0040e821,0x24042040,0x0c00044c,0,0x00402021,
            0x24030022,12,0x24030032,12,0x8c022080,0x2442004d,0x2403007b,12};
        for (unsigned i=0;i<std::size(sleep_main);++i) word(0x100+i*4,sleep_main[i]);
        word(0x200,0x24040001); word(0x204,0x24030033); word(0x208,12);
        word(0x20c,0x24020001); word(0x210,0xac022080); word(0x214,0x03e00008); word(0x218,0);
        word(0x220,0x24030023); word(0x224,12); word(0x228,0x2403007b); word(0x22c,12);
        word(0x230,0x24030020); word(0x234,12); word(0x238,0x03e00008); word(0x23c,0);
        word(0x344,0x1100); word(0x348,0x9000); word(0x34c,0x1000); word(0x354,10);
        nt::write_file(std::string(argv[2])+".sleep",elf);
        word(0x204,0x2403ffcc);
        nt::write_file(std::string(argv[2])+".iwake",elf);
        elf=original;
        word(92,0x1000); nt::write_file(std::string(argv[2])+".overlap",elf);
        word(92,0x2000); word(0x100,0x4c000000); nt::write_file(std::string(argv[2])+".unsupported",elf);
    }
    return output ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr<<"EE fixture generation: "<<error.what()<<'\n';
    return 1;
}
