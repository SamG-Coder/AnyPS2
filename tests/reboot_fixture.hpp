#pragma once
#include "disc_fixture.hpp"
#include <array>

namespace fixture {
inline nt::Bytes memory_bootstrap_image() {
    nt::Bytes module(140,0);
    const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(identity),std::end(identity),module.begin());
    module[16]=0x80; module[17]=0xff; module[18]=8; module[20]=1;
    put32(module,28,52); put32(module,32,84);
    module[40]=52; module[42]=32; module[44]=1; module[46]=40; module[48]=1;
    put32(module,52,1); put32(module,56,128); put32(module,68,12);
    put32(module,72,32); put32(module,76,7); put32(module,80,16);
    put32(module,128,0x00801021); // Return the supplied RAM size.
    put32(module,132,0x03e00008);
    nt::Bytes archive(80+module.size(),0);
    const std::array<std::string,4> names{"RESET","ROMDIR","EXTINFO","SYSMEM"};
    for (unsigned i=0;i<names.size();++i) std::copy(names[i].begin(),names[i].end(),archive.begin()+i*16);
    put32(archive,28,80); put32(archive,60,static_cast<std::uint32_t>(module.size()));
    std::copy(module.begin(),module.end(),archive.begin()+80);
    return archive;
}
inline nt::Bytes manager_bootstrap_image() {
    const auto original=memory_bootstrap_image();
    nt::Bytes sysmem(original.begin()+80,original.end());
    sysmem.resize(376,0);
    std::fill(sysmem.begin()+128,sysmem.begin()+256,0);
    put32(sysmem,32,256); sysmem[48]=2;
    put32(sysmem,68,128); put32(sysmem,72,160);
    put32(sysmem,24,48); put32(sysmem,176,0x00801021); put32(sysmem,180,0x03e00008);
    const std::uint32_t allocate[]{0x3c080001,0x8d020080,0,0x14400002,0,0x3c020002,
                                 0x3c090001,0x00494821,0xad090080,0x03e00008,0};
    for (unsigned i=0;i<std::size(allocate);++i) put32(sysmem,208+i*4,allocate[i]);
    put32(sysmem,128,0x41c00000); sysmem[136]=2; sysmem[137]=1;
    const std::string library="sysmem";
    std::copy(library.begin(),library.end(),sysmem.begin()+140);
    put32(sysmem,300,9); put32(sysmem,312,336); put32(sysmem,316,40); put32(sysmem,332,8);
    for (unsigned i=0;i<5;++i) {
        put32(sysmem,148+i*4,80); put32(sysmem,336+i*8,20+i*4); put32(sysmem,340+i*8,2);
    }
    nt::Bytes manager(original.begin()+80,original.end()); manager.resize(224,0);
    put32(manager,68,96); put32(manager,72,128);
    put32(manager,128,0x8c820000); put32(manager,132,0x3c08bf80); put32(manager,136,0x8d091450);
    put32(manager,144,0x8d0a1578); put32(manager,148,0); put32(manager,152,0x03e00008);
    put32(manager,160,0x41e00000); manager[168]=2; manager[169]=1;
    std::copy(library.begin(),library.end(),manager.begin()+172);
    put32(manager,180,0x03e00008); put32(manager,184,0x24000004);
    nt::Bytes archive(96+384+manager.size(),0);
    const std::array<std::string,5> names{"RESET","ROMDIR","EXTINFO","SYSMEM","LOADCORE"};
    for (unsigned i=0;i<names.size();++i) std::copy(names[i].begin(),names[i].end(),archive.begin()+i*16);
    put32(archive,28,96); put32(archive,60,static_cast<std::uint32_t>(sysmem.size()));
    put32(archive,76,static_cast<std::uint32_t>(manager.size()));
    std::copy(sysmem.begin(),sysmem.end(),archive.begin()+96);
    std::copy(manager.begin(),manager.end(),archive.begin()+480);
    return archive;
}
inline nt::Bytes manager_handshake_image() {
    auto archive=manager_bootstrap_image();
    // Original manager waits for SIF1 payload, then echoes it through SIF0 DMA.
    nt::Bytes imports(archive.begin()+480+160,archive.begin()+480+200);
    archive.resize(480+448,0);
    std::fill(archive.begin()+480+128,archive.end(),0);
    put32(archive,76,448); put32(archive,480+68,320); put32(archive,480+72,320);
    std::copy(imports.begin(),imports.end(),archive.begin()+480+400);
    constexpr std::uint32_t words[]{0x3c08bd00,0x8d090020,0,0x1120fffd,0,
        0x3c08bf80,0x34098000,0xad091570,0x3c094100,0x35290300,0xad091538,
        0x8c020800,0,0x1040fffd,0,
        0x34098800,0xad091570, // Enable both SIF channel priorities.
        0x3c098000,0x35290800,0xac090900,0x24090004,0xac090904,
        0x3c097000,0x35290001,0xac090908,0x24096000,0xac09090c,
        0x24090900,0xad09152c,0x3c090100,0x35290701,0xad091528,
        0x8d091528,0x3c0a0100,0x012a4824,0x1520fffc,0,0x03e00008,0};
    for (unsigned i=0;i<std::size(words);++i) put32(archive,480+128+4*i,words[i]);
    return archive;
}
inline nt::Bytes reboot_image() {
    nt::Bytes image(115,0);
    const auto entry=[&](unsigned at,const std::string& name,unsigned size,unsigned extra=0) {
        std::copy(name.begin(),name.end(),image.begin()+at);
        image[at+10]=static_cast<std::uint8_t>(extra); put32(image,at+12,size);
    };
    entry(0,"RESET",0);
    entry(16,"ROMDIR",80);
    entry(32,"EXTINFO",4);
    entry(48,"EXAMPLE",19,4);
    image[80]=1; image[83]=2; // Version metadata, no module code.
    for (unsigned i=96;i<115;++i) image[i]=static_cast<std::uint8_t>(i);
    return image;
}
}
