#include <nt/gs.hpp>

namespace nt::hardware {
void Gs::texture_register(unsigned reg, std::uint64_t value) {
    const unsigned format=(value>>20)&63, load=(value>>61)&7;
    const unsigned base=(value>>37)&16383, bank=(value>>56)&15;
    if (format!=20 || load==0 || load>=6) {drawing_[reg]=value;return;}
    const unsigned slot=(load==3 || load==5) ? 1:0;
    if (load>=4 && palette_base_[slot]==base) {drawing_[reg]=value;return;}
    const unsigned palette_format=(value>>51)&15;
    if (palette_format>1) throw runtime::MemoryFault("GS: 16-bit CLUT loading is not implemented");
    const bool storage2=(value>>55)&1;
    const auto config=drawing_[0x1c];
    const unsigned pitch=config&63, x=((config>>6)&63)*16, y=(config>>12)&1023;
    for (unsigned i=0;i<16;++i) {
        // CSM1 lays out the 16 entries as two eight-pixel rows in one block.
        const unsigned address=storage2 ? pixel_word(base,pitch,x+i,y) :
            (base*64+(i&1)+((i&6)<<1)+((i&8)>>2))&0xfffff;
        palette_[bank*16+i]=(*local_)[address];
    }
    if (load>=2) palette_base_[slot]=base;
    drawing_[reg]=value;
}
}
