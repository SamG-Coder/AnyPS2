#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include <unordered_map>

namespace nt::audio {
struct Sample {
    std::vector<std::int16_t> pcm;
    std::uint32_t pitch=0,loop_begin=0,loop_length=0;
};
enum class SampleError { none, malformed, unsupported, limit };
struct AdpcmHistory { int previous=0,older=0; };
inline SampleError decode_adpcm_block(std::span<const std::uint8_t> bytes,AdpcmHistory& history,std::vector<std::int16_t>& pcm) {
    if (bytes.size()!=16) return SampleError::malformed;
    const unsigned shift=bytes[0]&15,filter=bytes[0]>>4;
    if (filter>4 || shift>12 || bytes[1]>7) return SampleError::unsupported;
    constexpr int coefficients[5][2]={{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    for (unsigned nibble=0;nibble<28;++nibble) {
        const int raw=(bytes[2+nibble/2]>>((nibble%2)*4))&15;
        const int residual=((raw>=8 ? raw-16 : raw)*4096)>>shift;
        const int prediction=(history.previous*coefficients[filter][0]+history.older*coefficients[filter][1]+32)>>6;
        const auto value=std::clamp(residual+prediction,-32768,32767);
        pcm.push_back(static_cast<std::int16_t>(value));
        history.older=history.previous; history.previous=value;
    }
    return SampleError::none;
}
struct RawSample {
    Sample sample;
    std::uint32_t source_end=0,first_end_frame=0,repeat_address=0;
};
// Decode immutable sample data before handing PCM to the native engine. A loop
// is reusable only when its predictor history repeats, not merely its bytes.
inline SampleError decode_raw_sample(std::span<const std::uint8_t> bytes,std::uint32_t start,
                                    std::uint32_t repeat,RawSample& output) {
    constexpr std::size_t maximum_frames=4*1024*1024;
    if (start%16 || repeat%16 || start>=bytes.size()) return SampleError::malformed;
    RawSample decoded; AdpcmHistory history,loop_history;
    std::uint32_t loop=repeat,loop_frame=0;
    bool have_loop=false,ended=false,looping=false;
    for (std::size_t at=start;at+16<=bytes.size();at+=16) {
        if (decoded.sample.pcm.size()>maximum_frames-28) return SampleError::limit;
        if ((bytes[at+1]&4) || at==loop) {
            loop=static_cast<std::uint32_t>(at); have_loop=true;
            loop_frame=static_cast<std::uint32_t>(decoded.sample.pcm.size()); loop_history=history;
        }
        const auto result=decode_adpcm_block(bytes.subspan(at,16),history,decoded.sample.pcm);
        if (result!=SampleError::none) return result;
        if (bytes[at+1]&1) {
            decoded.source_end=static_cast<std::uint32_t>(at+16); ended=true;
            looping=(bytes[at+1]&2)!=0; break;
        }
    }
    if (!ended) return SampleError::malformed;
    decoded.first_end_frame=static_cast<std::uint32_t>(decoded.sample.pcm.size());
    if (looping) {
        if (!have_loop || loop<start || loop>=decoded.source_end) return SampleError::unsupported;
        const auto key=[](AdpcmHistory h) { return static_cast<std::uint32_t>(static_cast<std::uint16_t>(h.previous))|
            (static_cast<std::uint32_t>(static_cast<std::uint16_t>(h.older))<<16); };
        std::unordered_map<std::uint32_t,std::uint32_t> visited{{key(loop_history),loop_frame}};
        for (;;) {
            const auto [it,inserted]=visited.emplace(key(history),static_cast<std::uint32_t>(decoded.sample.pcm.size()));
            if (!inserted) {
                decoded.sample.loop_begin=it->second;
                decoded.sample.loop_length=static_cast<std::uint32_t>(decoded.sample.pcm.size())-it->second;
                break;
            }
            if (visited.size()>4096) return SampleError::limit;
            for (std::size_t at=loop;at<decoded.source_end;at+=16) {
                if (decoded.sample.pcm.size()>maximum_frames-28) return SampleError::limit;
                const auto result=decode_adpcm_block(bytes.subspan(at,16),history,decoded.sample.pcm);
                if (result!=SampleError::none) return result;
            }
        }
        decoded.repeat_address=loop;
    }
    output=std::move(decoded); return SampleError::none;
}
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
    AdpcmHistory history;
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
        const auto result=decode_adpcm_block(bytes.subspan(offset,16),history,sample.pcm);
        if (result!=SampleError::none) return result;
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
