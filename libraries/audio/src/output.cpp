#include <nt/audio.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <array>
#include <cmath>

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
        // Core zero feeds the external input of core one. Wet buses stay
        // disconnected from audible output until an effect is implemented.
        for (int core=1;core>=0;--core) {
            const auto sends=core==0 ? destinations(pending.mix[1],pending.wet[1]) : destinations(master_);
            auto result=engine_->CreateSubmixVoice(&pending.mix[core],2,48000,0,core*2+1,&sends.list);
            if (SUCCEEDED(result)) {
                const auto wet_send=destinations(pending.mix[core]);
                result=engine_->CreateSubmixVoice(&pending.wet[core],2,48000,0,core*2,&wet_send.list);
            }
            if (SUCCEEDED(result)) result=pending.wet[core]->SetVolume(0);
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
        graph_.clear(); graph_.swap(pending); error_=0;
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
            !valid(mix.external_effect) || !valid(mix.input_direct) || !valid(mix.input_effect)) return false;
        const float output[]{mix.output.left,mix.output.right};
        auto result=graph_.mix[core]->SetChannelVolumes(2,output);
        if (SUCCEEDED(result) && core==1) {
            const float direct[]{mix.external_direct.left,0,0,mix.external_direct.right};
            const float wet[]{mix.external_effect.left,0,0,mix.external_effect.right};
            result=graph_.mix[0]->SetOutputMatrix(graph_.mix[1],2,2,direct);
            if (SUCCEEDED(result)) result=graph_.mix[0]->SetOutputMatrix(graph_.wet[1],2,2,wet);
        }
        if (SUCCEEDED(result)) core_mix_[core]=mix;
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
        ~Graph() { clear(); }
        void swap(Graph& other) { voices.swap(other.voices); mix.swap(other.mix); wet.swap(other.wet); }
        void clear() {
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
    void close() override {}
    bool ready() const override { return false; }
    unsigned voices() const override { return 0; }
    std::uint32_t error() const override { return 1; }
};
}
std::unique_ptr<Output> make_output() { return std::make_unique<UnavailableOutput>(); }
}
#endif
