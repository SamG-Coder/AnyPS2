#include <nt/audio.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include "windows_reverb.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace nt::audio {
namespace {
class WindowsOutput final : public Output {
public:
    ~WindowsOutput() override { close(); }
    bool open() override {
        if (ready()) return true;
        auto result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if (FAILED(result)) { error_=static_cast<std::uint32_t>(result); return false; }
        com_=true;
        result=XAudio2Create(&engine_,0,XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(result)) result=engine_->CreateMasteringVoice(&master_,2,48000);
        if (FAILED(result)) { error_=static_cast<std::uint32_t>(result); close(); return false; }
        error_=0;
        return true;
    }
    bool initialize(bool) override {
        if (!open()) return false;
        Graph pending;
        // Core zero feeds the external input of core one. Each wet bus owns an
        // independent native reverb effect and starts with inaudible routing.
        for (int core=1;core>=0;--core) {
            const auto sends=core==0 ? destinations(pending.mix[1],pending.wet[1]) : destinations(master_);
            auto result=engine_->CreateSubmixVoice(&pending.mix[core],2,48000,0,core*2+1,&sends.list);
            if (SUCCEEDED(result)) {
                const auto wet_send=destinations(pending.mix[core]);
                result=engine_->CreateSubmixVoice(&pending.wet[core],2,48000,0,core*2,&wet_send.list);
            }
            if (SUCCEEDED(result)) result=replace_reverb(pending.wet[core],{});
            const float silent[4]{};
            if (SUCCEEDED(result)) result=pending.wet[core]->SetOutputMatrix(pending.mix[core],2,2,silent);
            if (FAILED(result)) { error_=static_cast<std::uint32_t>(result); return false; }
        }
        const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,48000,96000,2,16,0};
        for (unsigned index=0;index<pending.voices.size();++index) {
            auto& voice=pending.voices[index];
            const auto sends=destinations(pending.mix[index/24],pending.wet[index/24]);
            auto result=engine_->CreateSourceVoice(&voice,&format,0,4.0f,nullptr,&sends.list);
            const float silent[2]{};
            if (SUCCEEDED(result)) result=voice->SetOutputMatrix(pending.mix[index/24],1,2,silent);
            if (SUCCEEDED(result)) result=voice->SetOutputMatrix(pending.wet[index/24],1,2,silent);
            if (SUCCEEDED(result)) result=voice->SetFrequencyRatio(16383.0f/4096.0f);
            if (FAILED(result)) {
                error_=static_cast<std::uint32_t>(result); return false;
            }
        }
        const WAVEFORMATEX stereo{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        for (unsigned core=0;core<2;++core) {
            const auto sends=destinations(pending.mix[core],pending.wet[core]);
            const auto result=engine_->CreateSourceVoice(&pending.streams[core],&stereo,0,1.0f,nullptr,&sends.list);
            if (FAILED(result)) { error_=static_cast<std::uint32_t>(result); return false; }
        }
        graph_.clear(); graph_.swap(pending); core_mix_={}; error_=0;
        return true;
    }
    bool configure_voice(unsigned index,const VoiceMix& mix) override {
        if (index>=48 || !graph_.voices[index] || !valid(mix.direct) || !valid(mix.effect) ||
            !std::isfinite(mix.rate) || mix.rate<0 || mix.rate>4 || (mix.rate>0 && mix.rate<XAUDIO2_MIN_FREQ_RATIO)) return false;
        auto* voice=graph_.voices[index];
        const float direct[]{mix.direct.left,mix.direct.right},wet[]{mix.effect.left,mix.effect.right};
        auto result=voice->SetOutputMatrix(graph_.mix[index/24],1,2,direct);
        if (SUCCEEDED(result)) result=voice->SetOutputMatrix(graph_.wet[index/24],1,2,wet);
        // A zero source rate is stationary, not XAudio2's minimum rate.
        if (SUCCEEDED(result)) result=mix.rate==0 ? voice->Stop() : voice->SetFrequencyRatio(mix.rate);
        error_=static_cast<std::uint32_t>(result);
        return SUCCEEDED(result);
    }
    bool configure_core(unsigned core,const CoreMix& mix) override {
        if (core>=2 || !graph_.mix[core] || !valid(mix.output) || !valid(mix.external_direct) ||
            !valid(mix.external_effect) || !valid(mix.input_direct) || !valid(mix.input_effect) || !valid(mix.reverb.depth)) return false;
        XAUDIO2FX_REVERB_PARAMETERS validation{};
        if (!windows::reverb_parameters(mix.reverb.space,validation)) return false;
        const float output[]{mix.output.left,mix.output.right};
        auto result=graph_.mix[core]->SetChannelVolumes(2,output);
        if (SUCCEEDED(result) && core==1) {
            const float direct[]{mix.external_direct.left,0,0,mix.external_direct.right};
            const float wet[]{mix.external_effect.left,0,0,mix.external_effect.right};
            result=graph_.mix[0]->SetOutputMatrix(graph_.mix[1],2,2,direct);
            if (SUCCEEDED(result)) result=graph_.mix[0]->SetOutputMatrix(graph_.wet[1],2,2,wet);
        }
        if (SUCCEEDED(result)) result=configure_stream(core,mix);
        if (SUCCEEDED(result)) {
            if (core_mix_[core].reverb.space!=mix.reverb.space) result=replace_reverb(graph_.wet[core],mix.reverb);
            else result=effect_enabled(mix.reverb) ? graph_.wet[core]->EnableEffect(0) : graph_.wet[core]->DisableEffect(0);
        }
        if (SUCCEEDED(result)) {
            const float left=effect_enabled(mix.reverb) ? mix.reverb.depth.left : 0;
            const float right=effect_enabled(mix.reverb) ? mix.reverb.depth.right : 0;
            const float depth[]{left,0,0,right};
            result=graph_.wet[core]->SetOutputMatrix(graph_.mix[core],2,2,depth);
        }
        if (SUCCEEDED(result)) core_mix_[core]=mix;
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool clear_reverb(unsigned core) override {
        if (core>=2 || !graph_.wet[core]) return false;
        // Replacing the effect atomically discards its delay history. XAudio2
        // owns attached APOs; never reset one from the application thread.
        const auto result=replace_reverb(graph_.wet[core],core_mix_[core].reverb);
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool submit_stream(unsigned core,std::span<const std::int16_t> samples) override {
        if (core>=2 || !graph_.streams[core] || samples.empty() || samples.size()%2 || samples.size()>4*1024*1024) return false;
        StreamState state;
        if (!stream_state(core,state) || state.queued) return false;
        auto& storage=graph_.pcm[core]; storage.assign(samples.begin(),samples.end());
        // Keep the stream cursor cumulative across refill buffers. XAudio2
        // resets SamplesPlayed at END_OF_STREAM, obscuring the consumed count.
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=static_cast<UINT32>(storage.size()*sizeof(std::int16_t));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(storage.data());
        auto result=graph_.streams[core]->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result)) result=graph_.streams[core]->Start();
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool stream_state(unsigned core,StreamState& state) override {
        if (core>=2 || !graph_.streams[core]) return false;
        XAUDIO2_VOICE_STATE current{}; graph_.streams[core]->GetState(&current);
        state={current.SamplesPlayed,current.BuffersQueued!=0}; return true;
    }
    bool stop_stream(unsigned core) override {
        if (core>=2 || !graph_.streams[core]) return false;
        // DestroyVoice waits for the audio engine before owned PCM is released.
        graph_.streams[core]->DestroyVoice(); graph_.streams[core]=nullptr; graph_.pcm[core].clear();
        const WAVEFORMATEX stereo{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        const auto sends=destinations(graph_.mix[core],graph_.wet[core]);
        auto result=engine_->CreateSourceVoice(&graph_.streams[core],&stereo,0,1.0f,nullptr,&sends.list);
        if (SUCCEEDED(result)) result=configure_stream(core,core_mix_[core]);
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool stop_voice(unsigned index) override {
        if (index>=48 || !graph_.voices[index]) return false;
        auto result=graph_.voices[index]->Stop();
        if (SUCCEEDED(result)) result=graph_.voices[index]->FlushSourceBuffers();
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    void close() override {
        graph_.clear();
        if (master_) { master_->DestroyVoice(); master_=nullptr; }
        if (engine_) { engine_->Release(); engine_=nullptr; }
        if (com_) { CoUninitialize(); com_=false; }
    }
    bool ready() const override { return master_!=nullptr; }
    unsigned voices() const override { return graph_.voices.front() ? 48U : 0U; }
    std::uint32_t error() const override { return error_; }
private:
    static bool effect_enabled(const ReverbMix& mix) { return mix.enabled && mix.space!=ReverbSpace::off; }
    static HRESULT replace_reverb(IXAudio2SubmixVoice* voice,const ReverbMix& mix) {
        Microsoft::WRL::ComPtr<IUnknown> effect;
        XAUDIO2FX_REVERB_PARAMETERS parameters{};
        if (!windows::reverb_parameters(mix.space,parameters)) return E_INVALIDARG;
        auto result=XAudio2CreateReverb(effect.GetAddressOf());
        if (FAILED(result)) return result;
        XAUDIO2_EFFECT_DESCRIPTOR descriptor{effect.Get(),effect_enabled(mix),2};
        const XAUDIO2_EFFECT_CHAIN chain{1,&descriptor};
        result=voice->SetEffectChain(&chain);
        if (SUCCEEDED(result)) result=voice->SetEffectParameters(0,&parameters,sizeof(parameters));
        return result;
    }
    HRESULT configure_stream(unsigned core,const CoreMix& mix) {
        const float direct[]{mix.input_direct.left,0,0,mix.input_direct.right};
        const float wet[]{mix.input_effect.left,0,0,mix.input_effect.right};
        auto result=graph_.streams[core]->SetOutputMatrix(graph_.mix[core],2,2,direct);
        if (SUCCEEDED(result)) result=graph_.streams[core]->SetOutputMatrix(graph_.wet[core],2,2,wet);
        return result;
    }
    struct Sends {
        std::array<XAUDIO2_SEND_DESCRIPTOR,2> descriptors;
        XAUDIO2_VOICE_SENDS list;
        Sends(IXAudio2Voice* first,IXAudio2Voice* second) : descriptors{{{0,first},{0,second}}},list{second ? 2U : 1U,descriptors.data()} {}
    };
    static Sends destinations(IXAudio2Voice* first,IXAudio2Voice* second=nullptr) { return Sends(first,second); }
    static bool valid(StereoGain gain) { return std::isfinite(gain.left) && std::isfinite(gain.right) && std::abs(gain.left)<=1 && std::abs(gain.right)<=1; }
    struct Graph {
        std::array<IXAudio2SourceVoice*,48> voices{};
        std::array<IXAudio2SubmixVoice*,2> mix{},wet{};
        std::array<IXAudio2SourceVoice*,2> streams{};
        std::array<std::vector<std::int16_t>,2> pcm;
        ~Graph() { clear(); }
        void swap(Graph& other) { voices.swap(other.voices); mix.swap(other.mix); wet.swap(other.wet); streams.swap(other.streams); pcm.swap(other.pcm); }
        void clear() {
            for (auto& stream:streams) if (stream) { stream->DestroyVoice(); stream=nullptr; }
            for (auto& samples:pcm) samples.clear();
            for (auto& voice:voices) if (voice) { voice->DestroyVoice(); voice=nullptr; }
            for (unsigned core=0;core<2;++core) {
                if (wet[core]) { wet[core]->DestroyVoice(); wet[core]=nullptr; }
                if (mix[core]) { mix[core]->DestroyVoice(); mix[core]=nullptr; }
            }
        }
    };
    IXAudio2* engine_=nullptr;
    IXAudio2MasteringVoice* master_=nullptr;
    Graph graph_;
    std::array<CoreMix,2> core_mix_{};
    bool com_=false;
    std::uint32_t error_=0;
};
}
std::unique_ptr<Output> make_output() { return std::make_unique<WindowsOutput>(); }
}
#else
namespace nt::audio {
namespace {
class UnavailableOutput final : public Output {
public:
    bool open() override { return false; }
    bool initialize(bool) override { return false; }
    bool configure_voice(unsigned,const VoiceMix&) override { return false; }
    bool configure_core(unsigned,const CoreMix&) override { return false; }
    bool clear_reverb(unsigned) override { return false; }
    bool submit_stream(unsigned,std::span<const std::int16_t>) override { return false; }
    bool stream_state(unsigned,StreamState&) override { return false; }
    bool stop_stream(unsigned) override { return false; }
    bool stop_voice(unsigned) override { return false; }
    void close() override {}
    bool ready() const override { return false; }
    unsigned voices() const override { return 0; }
    std::uint32_t error() const override { return 1; }
};
}
std::unique_ptr<Output> make_output() { return std::make_unique<UnavailableOutput>(); }
}
#endif
