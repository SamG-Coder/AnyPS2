#pragma once

// Format address calculations shared by CPU import and LLVM GPU conversion.
namespace nt::graphics {
inline unsigned pack_rgba16(unsigned value) {
    unsigned packed=0;
    for (unsigned channel=0;channel<3;++channel) packed|=((value>>(channel*8+3))&31U)<<(channel*5);
    return packed|((value>>16)&0x8000U);
}
inline unsigned expand_rgba16(unsigned value) {
    unsigned rgba=(value&0x8000U)<<16;
    for (unsigned channel=0;channel<3;++channel) rgba|=((value>>(channel*5))&31U)<<(channel*8+3);
    return rgba;
}
inline unsigned rgba_halfword(unsigned base,unsigned pitch,unsigned x,unsigned y,bool alternate=false) {
    x&=2047; y&=2047;
    const auto page=(y/64)*pitch+x/64;
    // Both layouts use 16x8 blocks; PSMCT16S permutes the upper block bits.
    const auto block=((y>>3)&1)|(((x>>4)&1)<<1)|
        (alternate ? (((y>>5)&1)<<2)|(((y>>4)&1)<<3)|(((x>>5)&1)<<4) :
                     (((y>>4)&1)<<2)|(((x>>5)&1)<<3)|(((y>>5)&1)<<4));
    const auto half=((x>>3)&1)|((x&1)<<1)|((y&1)<<2)|((x&6)<<2)|((y&6)<<4);
    return (base*128+page*4096+block*128+half)&0x1fffff;
}
inline unsigned rgba_word(unsigned base,unsigned pitch,unsigned x,unsigned y) {
    x&=2047; y&=2047;
    const auto page=(y/32)*pitch+x/64;
    const auto block=((x>>3)&1)|(((y>>3)&1)<<1)|(((x>>4)&1)<<2)|(((y>>4)&1)<<3)|(((x>>5)&1)<<4);
    const auto word=(x&1)|((y&1)<<1)|((x&6)<<1)|((y&6)<<3);
    return (base*64+page*2048+block*64+word)&0xfffff;
}
// PSMZ32 and PSMZ24 keep PSMCT32's pixels within a block but use another block order.
inline unsigned stored_word(unsigned base,unsigned pitch,unsigned x,unsigned y,bool depth_layout) {
    return rgba_word(base,pitch,x,y)^(depth_layout ? 0x600U : 0U);
}
inline unsigned indexed_byte(unsigned base,unsigned pitch,unsigned x,unsigned y) {
    x&=2047; y&=2047;
    const auto page=(y/64)*(pitch/2)+x/128;
    const auto block=((x>>4)&1)|(((y>>4)&1)<<1)|(((x>>5)&1)<<2)|(((y>>5)&1)<<3)|(((x>>6)&1)<<4);
    const auto column=((x&1)<<2)|((x&2)<<3)|(((x>>2 ^ y>>1 ^ y>>2)&1)<<5)|
        ((x&8)>>2)|((y&1)<<3)|((y&2)>>1)|((y&4)<<4)|((y&8)<<4);
    return (base*256+page*8192+block*256+column)&0x3fffff;
}
inline unsigned indexed_nibble(unsigned base,unsigned pitch,unsigned x,unsigned y) {
    x&=2047; y&=2047;
    const auto page=(y/128)*(pitch/2)+x/128;
    const auto block=((y>>4)&1)|(((x>>5)&1)<<1)|(((y>>5)&1)<<2)|(((x>>6)&1)<<3)|(((y>>6)&1)<<4);
    const auto column=((x&1)<<3)|((x&2)<<4)|(((x>>2 ^ y>>1 ^ y>>2)&1)<<6)|
        ((x&8)>>2)|((x&16)>>2)|((y&1)<<4)|((y&2)>>1)|((y&4)<<5)|((y&8)<<5);
    return (base*512+page*16384+block*512+column)&0x7fffff;
}
}
