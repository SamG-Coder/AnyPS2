#pragma once
#include <cstdint>
#include <nt/audio_envelope.hpp>
#include <memory>
#include <span>

namespace nt::audio {
struct StereoGain { float left=0, right=0; };
struct VoiceMix {
    StereoGain direct, effect;
    float rate=1;
};
enum class ReverbSpace { off, room, studio_small, studio_medium, studio_large, hall, space, pipe };
struct ReverbMix {
    ReverbSpace space=ReverbSpace::off;
    StereoGain depth;
    bool enabled=false;
};
struct CoreMix {
    StereoGain output{1,1};
    StereoGain external_direct, external_effect;
    StereoGain input_direct, input_effect;
    ReverbMix reverb;
};
struct VoiceSample {
    unsigned voice=0;
    std::span<const std::int16_t> pcm;
    std::uint32_t loop_begin=0,loop_length=0;
    EnvelopeSpec envelope;
};
struct StreamState { std::uint64_t frames=0; bool queued=false; };
// Native output ownership. No console register file or instruction interface.
class Output {
public:
    virtual ~Output()=default;
    virtual bool open()=0;
    virtual bool initialize(bool preserve)=0;
    virtual bool configure_voice(unsigned voice,const VoiceMix& mix)=0;
    virtual bool configure_core(unsigned core,const CoreMix& mix)=0;
    virtual bool clear_reverb(unsigned core)=0;
    // One owned stereo PCM buffer per core; state comes from the audio engine.
    virtual bool submit_stream(unsigned core,std::span<const std::int16_t> samples)=0;
    virtual bool stream_state(unsigned core,StreamState& state)=0;
    virtual bool stop_stream(unsigned core)=0;
    // Owned mono PCM at the voice's 48 kHz base rate. A nonzero loop length
    // repeats that region; state is measured by the native audio engine.
    virtual bool submit_sample(unsigned,std::span<const std::int16_t>,std::uint32_t,std::uint32_t) { return false; }
    virtual bool sample_state(unsigned,StreamState&) { return false; }
    virtual bool start_voices(std::span<const VoiceSample>) { return false; }
    virtual bool configure_envelope(unsigned,const EnvelopeSpec&) { return false; }
    virtual bool release_voice(unsigned) { return false; }
    virtual bool envelope_state(unsigned,EnvelopeState&) { return false; }
    virtual bool stop_voice(unsigned voice)=0;
    virtual void close()=0;
    virtual bool ready() const=0;
    virtual unsigned voices() const=0;
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Output> make_output();
}
