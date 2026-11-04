#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace nt::media {
// BT.601 conversion with the IPU's coefficient precision and staged rounding.
// Output is RGBA with the console's 0x80 opaque alpha, before GS interpretation.
inline std::array<std::uint8_t,1024> mpeg_rgba(std::span<const std::uint8_t,384> planes,
                                             std::array<std::uint32_t,2> thresholds) {
    std::array<std::uint8_t,1024> rgba;
    for (unsigned pixel=0;pixel<256;++pixel) {
        const auto chroma=(pixel/32)*8+(pixel%16)/2;
        const int cb=int(planes[256+chroma])-128,cr=int(planes[320+chroma])-128;
        const int light=(149*std::max(0,int(planes[pixel])-16))>>6;
        const std::array<int,3> color{light+((204*cr)>>6),light+((-104*cr)>>6)+((-50*cb)>>6),light+((258*cb)>>6)};
        unsigned maximum=0;
        for (unsigned channel=0;channel<3;++channel) {
            const auto value=static_cast<std::uint8_t>(std::clamp((color[channel]+1)>>1,0,255));
            rgba[pixel*4+channel]=value; maximum=std::max(maximum,unsigned(value));
        }
        if (maximum<thresholds[0]) std::fill_n(rgba.begin()+pixel*4,4,0);
        else rgba[pixel*4+3]=maximum<thresholds[1] ? 64 : 128;
    }
    return rgba;
}
}
