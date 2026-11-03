#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace nt::audio {
// Quantized gain curves evaluated at the native 48 kHz processing rate.
// Configuration is converted from the public sound ABI, not read from devices.
struct GainCurve {
    std::uint32_t clock_step=32768;
    std::int32_t level_step=0;
    bool exponential=false;
};
struct EnvelopeSpec {
    std::array<GainCurve,4> curves{};
    std::uint32_t sustain_level=0;
};
struct EnvelopeState { std::uint32_t level=0; bool active=false; };
class Envelope {
public:
    void start() { phase_=0; level_=0; clock_=0; }
    void release() { if (phase_<4) { phase_=3; clock_=0; } }
    EnvelopeState state() const { return {static_cast<std::uint32_t>(level_),phase_<4}; }
    float next(const EnvelopeSpec& spec) {
        if (phase_>=4) return 0;
        const auto& curve=spec.curves[phase_];
        auto clock_step=curve.clock_step;
        auto level_step=curve.level_step;
        if (curve.exponential) {
            if (level_step>0 && level_>24576) clock_step/=4;
            if (level_step<0) level_step=static_cast<std::int32_t>((static_cast<std::int64_t>(level_step)*level_)>>15);
        }
        clock_+=std::max(1U,clock_step);
        if (clock_>=32768) { clock_=0; level_=std::clamp(level_+level_step,0,32767); }
        const auto gain=static_cast<float>(level_)/32768;
        if ((phase_==0 && level_==32767) || (phase_==1 && level_<=static_cast<int>(spec.sustain_level))) ++phase_;
        else if (phase_>=2 && level_==0) phase_=4;
        return gain;
    }
private:
    unsigned phase_=4;
    std::int32_t level_=0;
    std::uint32_t clock_=0;
};
}
