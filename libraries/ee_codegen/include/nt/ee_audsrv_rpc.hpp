#pragma once
#include <nt/ee_transfer_buffers.hpp>
#include <nt/audio.hpp>
#include <nt/adpcm.hpp>
#include <array>

namespace nt::ee {
// Public audsrv sample operations backed by owned PCM and native audio voices.
// Streaming/CD/callback operations are separate contracts and remain explicit
// unsupported boundaries until implemented; binding does not claim them.
class NativeAudioRpc final : public NativeRpcService {
public:
    NativeAudioRpc(NativeTransferBuffers& buffers,audio::Output& output)
        : NativeRpcService(0x0870884e,0x18000,0x19000),buffers_(buffers),output_(output) {}
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        last_method_=method;
        const unsigned input=method==0 ? 64 : method==0x17 || method==0x19 ? 12 : method==0x18 || method==0x1d ? 8 : 4;
        const unsigned size=method==0 ? 64 : method==0x17 ? 16 : 4;
        switch (method) {
        case 0: case 1: case 6: case 7: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1c: case 0x1d: break;
        default: return Stop::unsupported_system;
        }
        if (args.size()!=input || result_size!=size) return Stop::invalid_state;
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        std::array<std::uint32_t,16> response{};
        const auto value=le32(args,0);
        if (method!=0 && !initialized_) response[0]=1; // Public not-initialized error.
        else switch (method) {
        case 0:
            if (!initialized_) {
                if (!output_.initialize(false)) return Stop::unsupported_system;
                initialized_=true;
            }
            break;
        case 1:
            output_.close(); initialized_=adpcm_=false; samples_.clear(); channels_={};
            break;
        case 6:
            // STOP_AUDIO concerns the PCM stream, not the independently playing samples.
            if (!output_.stop_stream(0)) return Stop::unsupported_system;
            break;
        case 7: {
            if (value>0x3fff) { response[0]=5; break; }
            audio::CoreMix mix; mix.output={value/16383.0f,value/16383.0f};
            if (!output_.configure_core(1,mix)) return Stop::unsupported_system;
            break;
        }
        case 0x16:
            for (unsigned i=0;i<channels_.size();++i) {
                if (!output_.stop_voice(24+i) || !output_.configure_voice(24+i,{{1,1},{},1})) return Stop::unsupported_system;
            }
            channels_={}; samples_.clear(); adpcm_=true;
            break;
        case 0x17: {
            if (!adpcm_) { response[0]=1; break; }
            const auto bytes=buffers_.view(value,le32(args,4));
            const auto id=le32(args,8);
            if (!id || bytes.empty() || samples_.contains(id)) { response[0]=5; break; }
            audio::Sample sample;
            const auto decoded=audio::decode_apcm(bytes,sample);
            if (decoded==audio::SampleError::unsupported) return Stop::unsupported_system;
            if (decoded!=audio::SampleError::none) { response[0]=decoded==audio::SampleError::limit ? 4 : 5; break; }
            std::size_t total=sample.pcm.size();
            for (const auto& [key,existing]:samples_) total+=existing.pcm.size();
            if (samples_.size()>=128 || total>8*1024*1024) { response[0]=4; break; }
            response[1]=sample.pitch; response[2]=sample.loop_length!=0; response[3]=1;
            samples_.emplace(id,std::move(sample)); ++uploads_;
            break;
        }
        case 0x18: {
            if (!adpcm_) { response[0]=1; break; }
            const auto id=le32(args,4);
            const auto found=samples_.find(id);
            if (found==samples_.end()) { response[0]=5; break; }
            unsigned channel=value;
            if (channel>=channels_.size()) {
                for (channel=0;channel<channels_.size();++channel) {
                    audio::StreamState state;
                    if (!output_.sample_state(channel+24,state)) return Stop::unsupported_system;
                    if (!state.queued) break;
                }
            }
            audio::StreamState state;
            if (channel==channels_.size()) { response[0]=std::uint32_t(-7); break; }
            if (!output_.sample_state(channel+24,state)) return Stop::unsupported_system;
            if (state.queued) { response[0]=std::uint32_t(-7); break; }
            auto& voice=channels_[channel]; const auto& sample=found->second;
            const audio::VoiceMix mix{voice.gain,{},sample.pitch/4096.0f};
            if (!output_.configure_voice(channel+24,mix) ||
                !output_.submit_sample(channel+24,sample.pcm,sample.loop_begin,sample.loop_length)) return Stop::unsupported_system;
            voice.id=id; voice.rate=mix.rate; response[0]=channel; ++plays_;
            break;
        }
        case 0x19: {
            const auto left=le32(args,4),right=le32(args,8);
            if (!adpcm_ || value>=channels_.size() || left>0x3fff || right>0x3fff) { response[0]=5; break; }
            auto& voice=channels_[value]; const audio::StereoGain gain{left/16383.0f,right/16383.0f};
            if (!output_.configure_voice(value+24,{gain,{},voice.rate})) return Stop::unsupported_system;
            voice.gain=gain; break;
        }
        case 0x1c:
            // XAudio2 owns a separate copy until playback ends or is stopped.
            samples_.erase(value); break;
        case 0x1d: {
            const auto id=le32(args,4);
            if (value>=channels_.size()) { response[0]=5; break; }
            audio::StreamState state;
            if (!output_.sample_state(value+24,state)) return Stop::unsupported_system;
            if (!state.queued) break;
            if (!samples_.contains(id)) { response[0]=5; break; }
            response[0]=channels_[value].id==id;
            break;
        }
        }
        std::memcpy(memory.data+runtime::default_ram_address(destination),response.data(),size);
        return Stop::budget;
    }
    std::uint32_t last_method() const { return last_method_; }
    std::uint64_t uploads() const { return uploads_; }
    std::uint64_t plays() const { return plays_; }
    bool playback_state(audio::StreamState& state) {
        state={};
        if (!adpcm_) return true;
        for (unsigned i=0;i<channels_.size();++i) {
            audio::StreamState current;
            if (!output_.sample_state(i+24,current)) return false;
            state.frames+=current.frames; state.queued|=current.queued;
        }
        return true;
    }
private:
    struct Channel { std::uint32_t id=0; audio::StereoGain gain{1,1}; float rate=1; };
    NativeTransferBuffers& buffers_;
    audio::Output& output_;
    std::map<std::uint32_t,audio::Sample> samples_;
    std::array<Channel,24> channels_{};
    bool initialized_=false,adpcm_=false;
    std::uint32_t last_method_=0;
    std::uint64_t uploads_=0,plays_=0;
};
}
