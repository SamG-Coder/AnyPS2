#pragma once
#include <cstdint>
#include <memory>

namespace nt::audio {
struct StereoGain { float left=0, right=0; };
struct VoiceMix {
    StereoGain direct, effect;
    float rate=1;
};
struct CoreMix {
    StereoGain output{1,1};
    StereoGain external_direct, external_effect;
    StereoGain input_direct, input_effect;
};
// Native output ownership. No console register file or instruction interface.
class Output {
public:
    virtual ~Output()=default;
    virtual bool open()=0;
    virtual bool initialize(bool preserve)=0;
    virtual bool configure_voice(unsigned voice,const VoiceMix& mix)=0;
    virtual bool configure_core(unsigned core,const CoreMix& mix)=0;
    virtual void close()=0;
    virtual bool ready() const=0;
    virtual unsigned voices() const=0;
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Output> make_output();
}
