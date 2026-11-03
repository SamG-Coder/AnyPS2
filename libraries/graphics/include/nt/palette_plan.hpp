#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct PaletteLoad {
    unsigned bank;
    std::vector<unsigned> addresses;
};
// Plan palette reads without reading image storage.
// The caller applies the reads in submission order and commits bases atomically.
inline std::optional<PaletteLoad> plan_palette(std::uint64_t texture,std::uint64_t config,
    std::array<unsigned,2>& bases) {
    const unsigned format=(texture>>20)&63,load=(texture>>61)&7;
    const unsigned base=(texture>>37)&16383,bank=(texture>>56)&15;
    if ((format!=19 && format!=20) || load==0 || load>=6) return {};
    const unsigned slot=(load==3 || load==5) ? 1:0;
    if (load>=4 && bases[slot]==base) return {};
    if (((texture>>51)&15)>1) throw std::invalid_argument("16-bit palette loading is unsupported");
    const bool storage2=(texture>>55)&1;
    if (format==19 && storage2 && bank)
        throw std::invalid_argument("PSMT8 CSM2 palette load with nonzero CSA is unsupported");
    const unsigned pitch=config&63,x=((config>>6)&63)*16,y=(config>>12)&1023;
    PaletteLoad result{bank,{}};
    const unsigned count=format==19 ? 256-bank*16 : 16;
    result.addresses.resize(count);
    for (unsigned i=0;i<count;++i) {
        const unsigned index=format==19 ? bank*16+i : i;
        result.addresses[i]=storage2 ? rgba_word(base,pitch,x+i,y) :
            (base*64+(index&1)+((index&6)<<1)+((index&8)>>2)+
             ((index&16)<<2)+((index&96)>>1)+(index&128))&0xfffff;
    }
    if (load>=2) bases[slot]=base;
    return result;
}
inline void load_palette(std::array<std::uint32_t,256>& palette,
    std::span<const std::uint32_t> pixels,const PaletteLoad& load) {
    if (pixels.size()!=1024*1024 || load.bank>=16 || load.addresses.size()>256-load.bank*16)
        throw std::invalid_argument("Invalid native palette storage");
    for (const auto address:load.addresses)
        if (address>=pixels.size()) throw std::invalid_argument("Invalid native palette source");
    for (unsigned i=0;i<load.addresses.size();++i) palette[load.bank*16+i]=pixels[load.addresses[i]];
}
inline void snapshot_palette(std::span<std::uint32_t> output,
    const std::array<std::uint32_t,256>& palette,unsigned bank) {
    if (bank>=16 || (output.size()!=16 && output.size()!=256))
        throw std::invalid_argument("Invalid native palette snapshot");
    // Eight-bit indices select sixteen-color banks; CSA clamps the bank,
    // while the low four index bits retain their position within that bank.
    for (unsigned i=0;i<output.size();++i)
        output[i]=palette[std::min(bank+i/16,15U)*16+(i&15)];
}
}
