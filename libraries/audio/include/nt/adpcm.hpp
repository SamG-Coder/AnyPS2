#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace nt::audio {
struct Sample {
    std::vector<std::int16_t> pcm;
    std::uint32_t pitch=0,loop_begin=0,loop_length=0;
};
enum class SampleError { none, malformed, unsupported, limit };
// Decode data, never executable instructions. Publish only a completely
// validated sample. Predictor-independent loop starts can repeat native PCM.
inline SampleError decode_apcm(std::span<const std::uint8_t> bytes,Sample& output) {
    if (bytes.size()>2*1024*1024) return SampleError::limit;
    if (bytes.size()<32 || bytes.size()%16) return SampleError::malformed;
    const auto word=[&](unsigned n) {
        std::uint32_t value=0;
        for (unsigned i=0;i<4;++i) value|=std::uint32_t(bytes[n+i])<<(8*i);
        return value;
    };
    if (word(0)!=0x4d435041 || word(12)) return SampleError::malformed;
    if (bytes[4]!=1 || bytes[5]!=1 || bytes[6]>1 || bytes[7]) return SampleError::unsupported;
    Sample sample; sample.pitch=word(8);
    if (!sample.pitch || sample.pitch>16383) return SampleError::unsupported;
    const bool looping=bytes[6]!=0;
    bool loop_found=false,ended=false,independent=false;
    int previous=0,older=0;
    constexpr int coefficients[5][2]={{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    for (std::size_t offset=16;offset<bytes.size();offset+=16) {
        const unsigned shift=bytes[offset]&15,filter=bytes[offset]>>4,flags=bytes[offset+1];
        if (filter>4 || shift>12 || flags>7) return SampleError::unsupported;
        if (ended) {
            // Encoders can append one end/silence block after the playable data.
            if (offset+16!=bytes.size() || flags!=7 ||
                std::any_of(bytes.begin()+offset+2,bytes.end(),[](auto b) { return b!=0; })) return SampleError::unsupported;
            break;
        }
        if (flags&4) {
            if (loop_found) return SampleError::unsupported;
            loop_found=true; independent=filter==0;
            sample.loop_begin=static_cast<std::uint32_t>(sample.pcm.size());
        }
        for (unsigned nibble=0;nibble<28;++nibble) {
            const int raw=(bytes[offset+2+nibble/2]>>((nibble%2)*4))&15;
            const int residual=((raw>=8 ? raw-16 : raw)*4096)>>shift;
            const int prediction=(previous*coefficients[filter][0]+older*coefficients[filter][1]+32)>>6;
            const auto value=std::clamp(residual+prediction,-32768,32767);
            sample.pcm.push_back(static_cast<std::int16_t>(value));
            older=previous; previous=value;
        }
        if (flags&1) {
            ended=true;
            if (looping!=bool(flags&2)) return SampleError::unsupported;
        }
    }
    if (!ended) return SampleError::malformed;
    if (looping) {
        if (!loop_found || !independent) return SampleError::unsupported;
        sample.loop_length=static_cast<std::uint32_t>(sample.pcm.size())-sample.loop_begin;
    }
    else sample.loop_begin=0;
    output=std::move(sample); return SampleError::none;
}
}
