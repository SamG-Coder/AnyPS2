#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace nt::audio {
namespace detail {
struct PitchFactors {
    std::array<std::uint32_t,12> semitone{};
    std::array<std::uint32_t,128> fine{};
    PitchFactors() {
        for (unsigned i=0;i<semitone.size();++i)
            semitone[i]=static_cast<std::uint32_t>(std::exp2(static_cast<double>(i)/12)*32768);
        for (unsigned i=0;i<fine.size();++i)
            fine[i]=static_cast<std::uint32_t>(std::exp2(static_cast<double>(i)/1536)*32768);
    }
};
inline const PitchFactors& pitch_factors() { static const PitchFactors factors; return factors; }
}
// Public note units: 128 fine steps per semitone, 4096 pitch units at unity.
// Preserve factor quantization before octave rounding; a single exp2 differs.
inline std::optional<std::uint16_t> note_to_pitch(std::uint16_t center,std::uint16_t center_fine,
                                                std::uint16_t note,std::int16_t fine) {
    if (center>127 || note>127 || center_fine>127 || fine < -128 || fine>127) return {};
    int steps=(static_cast<int>(note)-center)*128+center_fine+fine;
    int octave=steps/1536, remainder=steps%1536;
    if (remainder<0) { --octave; remainder+=1536; }
    const int shift=2-octave;
    if (shift<1 || shift>31) return {};
    const auto& factors=detail::pitch_factors();
    const auto product=(static_cast<std::uint64_t>(factors.semitone[remainder/128])*factors.fine[remainder%128])>>16;
    return static_cast<std::uint16_t>((product+(1ULL<<(shift-1)))>>shift);
}
inline std::optional<std::uint16_t> pitch_to_note(std::uint16_t center,std::uint16_t center_fine,std::uint16_t pitch) {
    if (center>127 || center_fine>127 || !pitch) return {};
    pitch=std::min<std::uint16_t>(pitch,0x3fff);
    const int octave_bit=static_cast<int>(std::bit_width(pitch))-1;
    const auto normalized=static_cast<std::uint32_t>(pitch)<<(15-octave_bit);
    const auto& factors=detail::pitch_factors();
    const auto semitone=static_cast<unsigned>(std::upper_bound(factors.semitone.begin(),factors.semitone.end(),normalized)-factors.semitone.begin()-1);
    const auto fractional=(normalized*32768U)/factors.semitone[semitone];
    const auto fine=static_cast<unsigned>(std::upper_bound(factors.fine.begin(),factors.fine.end(),fractional)-factors.fine.begin()-1);
    const auto adjusted=center_fine+fine+1;
    const int note=static_cast<int>(center)+static_cast<int>(semitone)+(octave_bit-12)*12+static_cast<int>(adjusted/128);
    return static_cast<std::uint16_t>(note*256+static_cast<int>(adjusted&126));
}
}
