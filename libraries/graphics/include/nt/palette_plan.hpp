#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct PaletteLoad {
    unsigned bank;
    std::vector<unsigned> addresses;
    std::vector<unsigned> shifts;
    bool narrow=false;
};
inline bool palette_is16(std::uint64_t texture) {
    const auto format=(texture>>51)&15;
    return format==2 || format==10;
}
// Plan palette reads without reading image storage.
// The caller applies the reads in submission order and commits bases atomically.
inline std::optional<PaletteLoad> plan_palette(std::uint64_t texture,std::uint64_t config,
    std::array<unsigned,2>& bases) {
    const unsigned format=(texture>>20)&63,load=(texture>>61)&7;
    const bool narrow=palette_is16(texture);
    const unsigned base=(texture>>37)&16383,bank=(texture>>56)&(narrow ? 31 : 15);
    if ((format!=19 && format!=20) || load==0 || load>=6) return {};
    const unsigned slot=(load==3 || load==5) ? 1:0;
    if (load>=4 && bases[slot]==base) return {};
    if (((texture>>51)&15)>1 && !narrow) throw std::invalid_argument("Unsupported palette format");
    const bool storage2=(texture>>55)&1;
    if (format==19 && storage2 && bank)
        throw std::invalid_argument("PSMT8 CSM2 palette load with nonzero CSA is unsupported");
    if (narrow && storage2 && bank)
        throw std::invalid_argument("16-bit CSM2 palette load requires zero CSA");
    const unsigned pitch=config&63,x=((config>>6)&63)*16,y=(config>>12)&1023;
    PaletteLoad result{bank,{}};
    const unsigned count=format==19 ? (narrow ? 256 : 256-bank*16) : 16;
    if (narrow && bank*16+count>512)
        throw std::invalid_argument("16-bit palette load crossing palette capacity is unsupported");
    result.narrow=narrow;
    result.addresses.resize(count);
    if (narrow) result.shifts.resize(count);
    for (unsigned i=0;i<count;++i) {
        if (narrow) {
            // CSM1 palette slots use contiguous blocks, independent of the
            // image page stride; CSM2 uses ordinary image coordinates.
            const unsigned half=storage2 ? rgba_halfword(base,pitch,x+i,y,((texture>>51)&15)==10) :
                (base*128+(i&~31U)+((i&1)<<1)+((i&6)<<2)+((i&8)>>1)+((i&16)>>4))&0x1fffff;
            result.addresses[i]=half/2; result.shifts[i]=(half%2)*16;
            continue;
        }
        const unsigned index=format==19 ? bank*16+i : i;
        result.addresses[i]=storage2 ? rgba_word(base,pitch,x+i,y) :
            (base*64+(index&1)+((index&6)<<1)+((index&8)>>2)+
             ((index&16)<<2)+((index&96)>>1)+(index&128))&0xfffff;
    }
    if (load>=2) bases[slot]=base;
    return result;
}
inline void apply_palette(std::array<std::uint32_t,256>& palette,
    std::span<const std::uint32_t> words,const PaletteLoad& load) {
    const unsigned capacity=load.narrow ? 512 : 256;
    if (load.bank>=capacity/16 || words.size()!=load.addresses.size() || words.size()>capacity-load.bank*16 ||
        (load.narrow && load.shifts.size()!=words.size()))
        throw std::invalid_argument("Invalid native palette storage");
    if (load.narrow && std::any_of(load.shifts.begin(),load.shifts.end(),[](unsigned shift) { return shift!=0 && shift!=16; }))
        throw std::invalid_argument("Invalid native palette half-word selection");
    for (unsigned i=0;i<words.size();++i) {
        const unsigned index=load.bank*16+i;
        if (load.narrow) {
            // The two halves of a 32-bit palette occupy separate 256-entry
            // planes. Preserve this alias when loading a 16-bit palette bank.
            const unsigned shift=(index/256)*16;
            auto& target=palette[index%256];
            target=(target&~(65535U<<shift))|(((words[i]>>load.shifts[i])&65535U)<<shift);
        } else palette[index]=words[i];
    }
}
inline void load_palette(std::array<std::uint32_t,256>& palette,
    std::span<const std::uint32_t> pixels,const PaletteLoad& load) {
    if (pixels.size()!=1024*1024 || load.addresses.size()>256)
        throw std::invalid_argument("Invalid native palette storage");
    for (const auto address:load.addresses)
        if (address>=pixels.size()) throw std::invalid_argument("Invalid native palette source");
    std::array<std::uint32_t,256> words{};
    for (unsigned i=0;i<load.addresses.size();++i) words[i]=pixels[load.addresses[i]];
    apply_palette(palette,std::span(words).first(load.addresses.size()),load);
}
inline void snapshot_palette(std::span<std::uint32_t> output,
    const std::array<std::uint32_t,256>& palette,unsigned bank,bool narrow=false,std::uint64_t texa=0) {
    if (bank>=(narrow ? 32U : 16U) || (output.size()!=16 && output.size()!=256) ||
        (narrow && bank*16+output.size()>512))
        throw std::invalid_argument("Invalid native palette snapshot");
    if (narrow) {
        for (unsigned i=0;i<output.size();++i) {
            const unsigned index=bank*16+i;
            const unsigned value=(palette[index%256]>>((index/256)*16))&65535;
            const unsigned alpha=((texa&0x8000) && value==0) ? 0 : (texa>>((value&0x8000) ? 32 : 0))&255;
            output[i]=((value&31)<<3)|((value&992)<<6)|((value&31744)<<9)|(alpha<<24);
        }
        return;
    }
    // Eight-bit indices select sixteen-color banks; CSA clamps the bank,
    // while the low four index bits retain their position within that bank.
    for (unsigned i=0;i<output.size();++i)
        output[i]=palette[std::min(bank+i/16,15U)*16+(i&15)];
}
}
