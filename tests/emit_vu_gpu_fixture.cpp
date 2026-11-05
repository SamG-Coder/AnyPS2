#include <nt/vu_codegen.hpp>
#include <array>
#include <sstream>
#include <stdexcept>
#include <string>

// Original synthetic VU1 program for comparing the CPU and Vulkan targets of
// the VU block generator. Writes <prefix>.cpu.ll and <prefix>.gpu.ll.
namespace {
constexpr std::uint32_t upper_nop=0x2ffU,lower_nop=0x8000033cU,end_bit=0x40000000U,immediate_bit=0x80000000U;
// Field helpers. Special (11-bit) opcodes occupy the fd and function fields.
std::uint32_t upper(unsigned function,unsigned mask,unsigned ft,unsigned fs,unsigned fd) {
    return (mask<<21)|(ft<<16)|(fs<<11)|(fd<<6)|function;
}
std::uint32_t upper_special(unsigned opcode,unsigned mask,unsigned ft,unsigned fs) {
    return (mask<<21)|(ft<<16)|(fs<<11)|((opcode>>2)<<6)|(0x3cU|(opcode&3U));
}
std::uint32_t lower_register(unsigned function,unsigned t,unsigned s,unsigned d) {
    return 0x80000000U|(t<<16)|(s<<11)|(d<<6)|function;
}
std::uint32_t lower_special(unsigned opcode,unsigned field,unsigned t,unsigned s) {
    return 0x80000000U|(field<<21)|(t<<16)|(s<<11)|((opcode>>2)<<6)|(0x3cU|(opcode&3U));
}
std::uint32_t lower_immediate(unsigned opcode,unsigned field,unsigned t,unsigned s,int immediate) {
    return (opcode<<25)|(field<<21)|(t<<16)|(s<<11)|(static_cast<std::uint32_t>(immediate)&0x7ffU);
}
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    nt::Bytes bytes;
    const auto pair=[&](std::uint32_t lower,std::uint32_t upper_word=upper_nop) {
        for (const auto word:{lower,upper_word}) for (unsigned byte=0;byte<4;++byte)
            bytes.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
    };
    const auto here=[&] { return static_cast<int>(bytes.size()/8); };
    // Entry 0: transform VI2 vertices from VI1 to VI3 (VF10-VF13 rows), with
    // perspective Q, clip flags, and two XGKICKs (VI5 mid-program, VI9 at E).
    const auto transform=here();
    pair(lower_special(0x34,15,4,1));                                  // LQI VF4, (VI1++)
    pair(lower_nop,upper_special(0x18,15,4,10));                       // MULAx ACC, VF10, VF4x
    pair(lower_nop,upper_special(0x09,15,4,11));                       // MADDAy ACC, VF11, VF4y
    pair(lower_nop,upper_special(0x0a,15,4,12));                       // MADDAz ACC, VF12, VF4z
    pair(lower_nop,upper(0x0b,15,4,13,5));                             // MADDw VF5, VF13, VF4w
    pair(lower_special(0x38,(3U<<2)|3U,5,0),upper_special(0x1f,0,5,5)); // DIV Q, VF0w, VF5w; CLIP VF5, VF5w
    pair(lower_special(0x3b,0,0,0));                                   // WAITQ
    pair(lower_immediate(0x1c,0,7,0,0),upper(0x1c,15,0,5,6));          // FCGET VI7; MULq VF6, VF5, Q
    pair(lower_register(0x32,2,2,31),upper_special(0x15,15,7,6));      // IADDI VI2, VI2, -1; FTOI4 VF7, VF6
    pair(lower_special(0x35,15,3,7));                                  // SQI VF7, (VI3++)
    pair(lower_immediate(0x29,0,0,2,transform-(here()+1)));            // IBNE VI2, VI0, transform
    pair(lower_immediate(0x05,8,7,6,0));                               // delay: ISW.x VI7, 0(VI6)
    pair(lower_special(0x6c,0,0,5));                                   // XGKICK VI5
    pair(lower_special(0x6c,0,0,9),upper_nop|end_bit);                 // XGKICK VI9, E
    pair(lower_immediate(0x08,0,8,8,1));                               // delay: IADDIU VI8, VI8, 1
    // Entry 15: I literal, EFU P results with WAITP, MTIR, outer product,
    // MAX/MINI/ABS, and BAL/JR through a delay slot.
    const auto efu=here();
    pair(0x40400000U,upper_nop|immediate_bit);                         // I = 3.0
    pair(lower_special(0x73,0,0,1),upper(0x1e,15,0,1,2));              // ERLENG P, VF1; MULi VF2, VF1, I
    pair(lower_special(0x3c,0,10,2),upper_special(0x10,15,3,20));      // MTIR VI10, VF2x; ITOF0 VF3, VF20
    pair(lower_special(0x7b,0,0,0),upper_special(0x2e,14,2,1));        // WAITP; OPMULA ACC, VF1, VF2
    pair(lower_special(0x64,15,12,0),upper(0x2e,14,1,2,11));           // MFP VF12, P; OPMSUB VF11, VF2, VF1
    pair(lower_special(0x7c,0,0,3),upper(0x2b,15,11,12,13));           // ESIN P, VF3x; MAX VF13, VF12, VF11
    pair(lower_register(0x30,2,10,11),upper(0x1f,15,0,13,14));         // IADD VI11, VI10, VI2; MINIi VF14, VF13, I
    const auto call=here();
    pair(lower_immediate(0x21,0,15,0,(call+4)-(call+1)),upper_special(0x1d,15,15,14)); // BAL VI15, +4; ABS VF15, VF14
    pair(lower_immediate(0x08,0,12,0,7),upper(0x01,15,1,15,16));       // delay: IADDIU VI12, VI0, 7; ADDy VF16, VF15, VF1y
    pair(lower_immediate(0x08,0,13,0,99),upper_nop|end_bit);           // return point: IADDIU VI13, VI0, 99, E
    pair(lower_nop);                                                   // delay
    pair(lower_special(0x7b,0,0,0),upper(0x2c,15,2,16,17));            // WAITP; SUB VF17, VF16, VF2
    pair(lower_special(0x64,15,18,0),upper_special(0x1b,15,0,17));     // MFP VF18, P; MULAw ACC, VF17, VF0w
    pair(lower_immediate(0x24,0,0,15,0));                              // JR VI15
    pair(lower_special(0x3f,4,11,12));                                 // delay: ISWR.y VI11, (VI12)
    // Entry 30: SQRT Q read before and after WAITQ, status and MAC flag
    // reads, wrapped memory offsets, clip set and read.
    const auto scheduled=here();
    pair(lower_special(0x39,0,1,0),upper(0x28,15,2,1,19));             // SQRT Q, VF1x; ADD VF19, VF1, VF2
    pair(lower_immediate(0x16,1,14,0,0x7ff),upper(0x25,15,0,19,20));   // FSAND VI14, 0xfff; MSUBq VF20, VF19, Q (old Q)
    pair(lower_immediate(0x1a,0,4,2,0));                               // FMAND VI4, VI2
    pair(lower_immediate(0x00,15,21,0,-1));                            // LQ VF21, -1(VI0): row 1023
    pair(lower_immediate(0x01,15,0,19,1023));                          // SQ VF19, 1023(VI0)
    pair(lower_immediate(0x11,0,0,0,0x123456&0x7ff)|(0x123456U&~0x7ffU)); // FCSET 0x123456
    pair(lower_special(0x3b,0,0,0),upper_special(0x1f,0,20,19));       // WAITQ; CLIP VF19, VF20w
    pair(lower_immediate(0x1c,0,7,0,0),upper(0x1c,15,0,19,22));        // FCGET VI7; MULq VF22, VF19, Q
    pair(lower_immediate(0x13,0,0,0,0)|0xfff000U,upper_nop|end_bit);   // FCOR 0xfff000, E
    pair(lower_nop);
    const std::array<unsigned,3> entries{static_cast<unsigned>(transform),static_cast<unsigned>(efu),static_cast<unsigned>(scheduled)};
    const auto program=nt::vu::frame(bytes,1,0);
    const std::string prefix=argv[1];
    for (const bool gpu:{false,true}) {
        std::ostringstream module;
        if (gpu) nt::vu::emit_gpu_program_llvm(module,program,1,entries,"nt_vu_gpu_fixture");
        else nt::vu::emit_program_llvm(module,program,1,entries,"nt_vu_gpu_fixture");
        const auto text=module.str();
        nt::write_file(prefix+(gpu ? ".gpu.ll" : ".cpu.ll"),nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    }
    // Unsupported semantics stop GPU generation exactly as they stop CPU generation.
    auto invalid=program;
    invalid[1].upper.function=48;
    std::ostringstream rejected;
    bool failed=false;
    try { nt::vu::emit_gpu_program_llvm(rejected,invalid,1,entries,"nt_vu_gpu_invalid"); }
    catch (const std::invalid_argument&) { failed=true; }
    return failed && rejected.str().empty() ? 0 : 1;
}
