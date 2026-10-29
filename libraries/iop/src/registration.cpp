#include <nt/iop_registration.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
Bytes registration_module(std::uint32_t exports,std::uint32_t completion,std::uint16_t version) {
    if (!exports || exports%4 || exports>=2*1024*1024 || !completion || completion%4 || completion>=2*1024*1024)
        throw std::runtime_error("IOP registration: invalid export or completion address");
    constexpr unsigned text_offset=256, text_size=176, metadata=160;
    Bytes file(text_offset+text_size,0);
    const auto word=[&](unsigned offset,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) file[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
    };
    const auto half=[&](unsigned offset,std::uint16_t value) {
        file[offset]=static_cast<std::uint8_t>(value); file[offset+1]=static_cast<std::uint8_t>(value>>8);
    };
    const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(identity),std::end(identity),file.begin());
    half(16,0xff80); half(18,8); word(20,1); word(28,52); word(32,116);
    half(40,52); half(42,32); half(44,2); half(46,40); half(48,1);
    // IOP module metadata precedes PT_LOAD, as required by the public IRX ABI.
    word(52,0x70000080); word(56,metadata); word(68,48); word(80,4);
    word(84,1); word(88,text_offset); word(100,text_size); word(104,text_size); word(108,7); word(112,16);
    word(metadata,0xffffffff); word(metadata+12,text_size); half(metadata+24,0x100);
    constexpr std::string_view name="AnyPS2_register";
    std::copy(name.begin(),name.end(),file.begin()+metadata+26);
    const std::uint32_t code[]{
        0x27bdffe0, // Argument-save area, plus a separate saved caller RA.
        0xafbf001c,
        0x3c040000|(exports>>16), 0x34840000|(exports&0xffff),
        0x04110020,0, // bal to import stub at text+148; nop
        0x1440000b,0, // Nonzero registration result reaches an explicit break.
        0x3c080000|(completion>>16),0x35080000|(completion&0xffff),
        0x24090001,0xad090000,
        0x8fbf001c,0x27bd0020,0x03e00008,0
    };
    for (unsigned i=0;i<std::size(code);++i) word(text_offset+i*4,code[i]);
    word(text_offset+72,13);
    word(text_offset+128,0x41e00000); half(text_offset+136,version);
    constexpr std::string_view library="loadcore";
    std::copy(library.begin(),library.end(),file.begin()+text_offset+140);
    word(text_offset+148,0x03e00008); word(text_offset+152,0x24000006);
    return file;
}
}
