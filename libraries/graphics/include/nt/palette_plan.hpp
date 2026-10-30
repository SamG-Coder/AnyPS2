#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct PaletteLoad {
    unsigned bank;
    std::array<unsigned,16> addresses;
};
// Plan the supported 16-entry palette load without reading image storage.
// The caller applies the reads in submission order and commits bases atomically.
inline std::optional<PaletteLoad> plan_palette(std::uint64_t texture,std::uint64_t config,
    std::array<unsigned,2>& bases) {
    const unsigned format=(texture>>20)&63,load=(texture>>61)&7;
    const unsigned base=(texture>>37)&16383,bank=(texture>>56)&15;
    if (format!=20 || load==0 || load>=6) return {};
    const unsigned slot=(load==3 || load==5) ? 1:0;
    if (load>=4 && bases[slot]==base) return {};
    if (((texture>>51)&15)>1) throw std::invalid_argument("16-bit palette loading is unsupported");
    const bool storage2=(texture>>55)&1;
    const unsigned pitch=config&63,x=((config>>6)&63)*16,y=(config>>12)&1023;
    PaletteLoad result{bank,{}};
    for (unsigned i=0;i<16;++i)
        result.addresses[i]=storage2 ? rgba_word(base,pitch,x+i,y) :
            (base*64+(i&1)+((i&6)<<1)+((i&8)>>2))&0xfffff;
    if (load>=2) bases[slot]=base;
    return result;
}
inline void load_palette(std::array<std::uint32_t,256>& palette,
    std::span<const std::uint32_t> pixels,const PaletteLoad& load) {
    if (pixels.size()!=1024*1024 || load.bank>=16)
        throw std::invalid_argument("Invalid native palette storage");
    for (const auto address:load.addresses)
        if (address>=pixels.size()) throw std::invalid_argument("Invalid native palette source");
    for (unsigned i=0;i<16;++i) palette[load.bank*16+i]=pixels[load.addresses[i]];
}
}
