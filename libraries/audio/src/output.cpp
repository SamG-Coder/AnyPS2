#include <nt/audio.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include "windows_reverb.hpp"
#include "windows_envelope.hpp"
#include <array>
#include <cmath>
#include <vector>
#include <deque>

namespace nt::audio {
namespace {
class WindowsOutput final : public Output,private IXAudio2EngineCallback {
public:
    ~WindowsOutput() override { close(); }
    bool open() override {
        if (ready()) return true;
        auto result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        // Window/input hosts can already own an STA. Acquire a balanced
        // reference in that apartment instead of trying to change its model.
        if (result==RPC_E_CHANGED_MODE) result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if (FAILED(result)) { error_=static_cast<std::uint32_t>(result); return false; }
        com_=true;
        critical_error_=0;
        result=XAudio2Create(&engine_,0,XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(result)) result=engine_->CreateMasteringVoice(&master_,2,48000);
        if (SUCCEEDED(result)) result=engine_->RegisterForCallbacks(this);
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
            auto result=engine_->CreateSubmixVoice(&pending.mix[core],2,48000,0,core*2+2,&sends.list);
            if (SUCCEEDED(result)) {
                const auto wet_send=destinations(pending.mix[core]);
                result=engine_->CreateSubmixVoice(&pending.wet[core],2,48000,0,core*2+1,&wet_send.list);
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
            auto result=engine_->CreateSubmixVoice(&pending.envelope[index],1,48000,0,0,&sends.list);
            const auto source_send=destinations(pending.envelope[index]);
            if (SUCCEEDED(result)) result=engine_->CreateSourceVoice(&voice,&format,0,4.0f,nullptr,&source_send.list);
            const float silent[2]{};
            if (SUCCEEDED(result)) result=pending.envelope[index]->SetOutputMatrix(pending.mix[index/24],1,2,silent);
            if (SUCCEEDED(result)) result=pending.envelope[index]->SetOutputMatrix(pending.wet[index/24],1,2,silent);
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
        graph_.clear(); graph_.swap(pending); core_mix_={}; voice_mix_={}; envelopes_={}; envelope_parameters_={}; error_=0;
        return true;
    }
    bool configure_voice(unsigned index,const VoiceMix& mix) override {
        if (index>=48 || !graph_.voices[index] || !valid(mix.direct) || !valid(mix.effect) ||
            !std::isfinite(mix.rate) || mix.rate<0 || mix.rate>4 || (mix.rate>0 && mix.rate<XAUDIO2_MIN_FREQ_RATIO)) return false;
        auto* voice=graph_.voices[index];
        auto* bus=graph_.envelope[index];
        const auto operation=operation_set();
        const float direct[]{mix.direct.left,mix.direct.right},wet[]{mix.effect.left,mix.effect.right};
        auto result=bus->SetOutputMatrix(graph_.mix[index/24],1,2,direct,operation);
        if (SUCCEEDED(result)) result=bus->SetOutputMatrix(graph_.wet[index/24],1,2,wet,operation);
        // Change source motion and its downstream hold atomically. The envelope
        // remains on the 48 kHz mix clock even while the source is stationary.
        if (SUCCEEDED(result)) result=mix.rate==0 ? voice->Stop(0,operation) : voice->SetFrequencyRatio(mix.rate,operation);
        auto parameters=envelope_parameters_[index]; parameters.hold=mix.rate==0;
        if (SUCCEEDED(result) && envelopes_[index])
            result=bus->SetEffectParameters(0,&parameters,sizeof(parameters),operation);
        if (SUCCEEDED(result) && mix.rate>0 && voice_mix_[index].rate==0) {
            XAUDIO2_VOICE_STATE state{}; voice->GetState(&state);
            if (state.BuffersQueued) result=voice->Start(0,operation);
        }
        if (SUCCEEDED(result)) result=engine_->CommitChanges(operation);
        if (SUCCEEDED(result)) { voice_mix_[index]=mix; envelope_parameters_[index]=parameters; }
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
    bool submit_sample(unsigned index,std::span<const std::int16_t> samples,std::uint32_t loop_begin,std::uint32_t loop_length) override {
        if (index>=48 || !graph_.voices[index] || samples.empty() || samples.size()>4*1024*1024 ||
            std::uint64_t(loop_begin)+loop_length>samples.size() || (!loop_length && loop_begin)) return false;
        StreamState state;
        if (!sample_state(index,state) || state.queued) return false;
        auto& storage=graph_.voice_pcm[index]; storage.assign(samples.begin(),samples.end());
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=static_cast<UINT32>(storage.size()*sizeof(std::int16_t));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(storage.data());
        buffer.LoopBegin=loop_begin; buffer.LoopLength=loop_length;
        buffer.LoopCount=loop_length ? XAUDIO2_LOOP_INFINITE : 0;
        auto result=graph_.voices[index]->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result) && voice_mix_[index].rate!=0) result=graph_.voices[index]->Start();
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool start_voices(std::span<const VoiceSample> samples) override {
        std::uint64_t mask=0;
        for (const auto& sample:samples) {
            if (sample.voice>=48 || !graph_.voices[sample.voice] || sample.pcm.empty() || sample.pcm.size()>4*1024*1024 ||
                std::uint64_t(sample.loop_begin)+sample.loop_length>sample.pcm.size() || (!sample.loop_length && sample.loop_begin) ||
                (mask&(1ULL<<sample.voice))) return false;
            mask|=1ULL<<sample.voice;
        }
        if (samples.empty()) return true;
        const auto operation=operation_set();
        HRESULT result=S_OK;
        for (const auto& sample:samples) {
            const auto index=sample.voice;
            if (!stop_voice(index)) { result=E_FAIL; break; }
            envelopes_[index]=std::make_shared<windows::EnvelopeObservation>();
            auto* effect=new windows::EnvelopeEffect(envelopes_[index]);
            XAUDIO2_EFFECT_DESCRIPTOR descriptor{static_cast<IXAPO*>(effect),TRUE,1};
            const XAUDIO2_EFFECT_CHAIN chain{1,&descriptor};
            result=graph_.envelope[index]->SetEffectChain(&chain); effect->Release();
            if (FAILED(result)) break;
            envelope_parameters_[index]={sample.envelope,false,true,voice_mix_[index].rate==0};
            result=graph_.envelope[index]->SetEffectParameters(0,&envelope_parameters_[index],sizeof(windows::EnvelopeParameters),operation);
            if (FAILED(result)) break;
            auto& storage=graph_.voice_pcm[index]; storage.assign(sample.pcm.begin(),sample.pcm.end());
            graph_.voice_submitted[index]=storage.size();
            graph_.voice_loop[index]=sample.loop_length!=0;
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes=static_cast<UINT32>(storage.size()*sizeof(std::int16_t));
            buffer.pAudioData=reinterpret_cast<const BYTE*>(storage.data());
            buffer.LoopBegin=sample.loop_begin; buffer.LoopLength=sample.loop_length;
            buffer.LoopCount=sample.loop_length ? XAUDIO2_LOOP_INFINITE : 0;
            result=graph_.voices[index]->SubmitSourceBuffer(&buffer);
            if (SUCCEEDED(result) && voice_mix_[index].rate>0) result=graph_.voices[index]->Start(0,operation);
            if (FAILED(result)) break;
        }
        if (SUCCEEDED(result)) result=engine_->CommitChanges(operation);
        if (FAILED(result)) for (const auto& sample:samples) stop_voice(sample.voice);
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool append_voice(unsigned index,std::span<const std::int16_t> samples) override {
        if (index>=48 || !graph_.voices[index] || samples.empty() || samples.size()>4096) return false;
        StreamState state;
        if (!sample_state(index,state)) return false;
        auto& queued=graph_.voice_queue[index];
        while (!queued.empty() && queued.front().end<=state.frames) queued.pop_front();
        if (queued.size()>=32) return false;
        queued.push_back({{samples.begin(),samples.end()},graph_.voice_submitted[index]+samples.size()});
        const auto& storage=queued.back().pcm;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=static_cast<UINT32>(storage.size()*sizeof(std::int16_t));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(storage.data());
        const auto result=graph_.voices[index]->SubmitSourceBuffer(&buffer);
        if (SUCCEEDED(result)) graph_.voice_submitted[index]=queued.back().end;
        else queued.pop_back();
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool revise_voices(const VoiceRevisionBuilder& build) override {
        if (!engine_ || !build || critical_error_.load()) return false;
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event) { error_=GetLastError(); return false; }
        RevisionRequest request{build,event};
        RevisionRequest* empty=nullptr;
        if (!revision_.compare_exchange_strong(empty,&request)) { CloseHandle(event); return false; }
        if (critical_error_.load()) {
            auto* expected=&request;
            if (revision_.compare_exchange_strong(expected,nullptr)) SetEvent(event);
        }
        WaitForSingleObject(event,INFINITE); CloseHandle(event);
        return request.accepted;
    }
private:
    struct RevisionRequest {
        const VoiceRevisionBuilder& build;
        HANDLE complete;
        bool accepted=false;
    };
    void STDMETHODCALLTYPE OnProcessingPassStart() override {
        if (auto* request=revision_.exchange(nullptr)) {
            try { request->accepted=apply_revisions(request->build); }
            catch (...) { error_=static_cast<std::uint32_t>(E_FAIL); }
            SetEvent(request->complete);
        }
    }
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override {
        critical_error_.store(static_cast<std::uint32_t>(error));
        if (auto* request=revision_.exchange(nullptr)) SetEvent(request->complete);
    }
    bool apply_revisions(const VoiceRevisionBuilder& build) {
        std::array<StreamState,48> states;
        for (unsigned index=0;index<states.size();++index)
            if (!sample_state(index,states[index])) return false;
        std::vector<VoiceRevision> revisions;
        if (!build(states,revisions)) return false;
        if (revisions.size()>48) return false;
        struct Copy { std::span<std::int16_t> destination; std::span<const std::int16_t> source; };
        std::vector<Copy> copies;
        std::uint64_t seen=0;
        error_=static_cast<std::uint32_t>(E_INVALIDARG);
        for (const auto& revision:revisions) {
            const auto index=revision.voice;
            if (index>=48 || (seen&(1ULL<<index)) || graph_.voice_loop[index] ||
                revision.begin<states[index].frames || revision.begin>graph_.voice_submitted[index] ||
                revision.pcm.size()>graph_.voice_submitted[index]-revision.begin) return false;
            seen|=1ULL<<index;
            const auto end=revision.begin+revision.pcm.size();
            std::size_t copied=0;
            const auto select=[&](std::vector<std::int16_t>& pcm,std::uint64_t offset) {
                const auto first=std::max(offset,revision.begin),last=std::min(offset+pcm.size(),end);
                if (first>=last) return;
                const auto count=static_cast<std::size_t>(last-first);
                copies.push_back({std::span(pcm).subspan(static_cast<std::size_t>(first-offset),count),
                    std::span(revision.pcm).subspan(static_cast<std::size_t>(first-revision.begin),count)});
                copied+=count;
            };
            select(graph_.voice_pcm[index],0);
            for (auto& buffer:graph_.voice_queue[index]) select(buffer.pcm,buffer.end-buffer.pcm.size());
            if (copied!=revision.pcm.size()) return false;
        }
        // Engine callbacks run between processing passes. Neither source
        // cursors nor resamplers race these writes, and the graph stays intact.
        for (const auto& copy:copies) std::copy(copy.source.begin(),copy.source.end(),copy.destination.begin());
        error_=0; return true;
    }
public:
    bool configure_envelope(unsigned index,const EnvelopeSpec& spec) override {
        if (index>=48 || !graph_.voices[index]) return false;
        if (!envelopes_[index]) return true;
        auto parameters=envelope_parameters_[index]; parameters.spec=spec;
        const auto result=graph_.envelope[index]->SetEffectParameters(0,&parameters,sizeof(parameters));
        if (SUCCEEDED(result)) envelope_parameters_[index]=parameters;
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool release_voice(unsigned index) override {
        if (index>=48 || !graph_.voices[index]) return false;
        if (!envelopes_[index]) return stop_voice(index);
        auto parameters=envelope_parameters_[index]; parameters.release=true;
        const auto result=graph_.envelope[index]->SetEffectParameters(0,&parameters,sizeof(parameters));
        if (SUCCEEDED(result)) envelope_parameters_[index]=parameters;
        error_=static_cast<std::uint32_t>(result); return SUCCEEDED(result);
    }
    bool envelope_state(unsigned index,EnvelopeState& state) override {
        if (index>=48 || !graph_.voices[index]) return false;
        if (!envelopes_[index]) { state={}; return true; }
        const auto observed=envelopes_[index]->value.load(std::memory_order_relaxed);
        state={observed&0xffffU,(observed&0x10000)!=0};
        StreamState sample;
        if (!sample_state(index,sample)) return false;
        if (!sample.queued) state={};
        return true;
    }
    bool sample_state(unsigned index,StreamState& state) override {
        if (index>=48 || !graph_.voices[index]) return false;
        XAUDIO2_VOICE_STATE current{}; graph_.voices[index]->GetState(&current);
        state={current.SamplesPlayed,current.BuffersQueued!=0}; return true;
    }
    bool stop_voice(unsigned index) override {
        if (index>=48 || !graph_.voices[index]) return false;
        // Wait for the engine before releasing/replacing its sample storage.
        graph_.voices[index]->DestroyVoice(); graph_.voices[index]=nullptr;
        const auto detached=graph_.envelope[index]->SetEffectChain(nullptr);
        if (FAILED(detached)) { error_=static_cast<std::uint32_t>(detached); return false; }
        envelopes_[index].reset(); envelope_parameters_[index]={};
        graph_.voice_pcm[index].clear();
        graph_.voice_queue[index].clear(); graph_.voice_submitted[index]=0;
        graph_.voice_loop[index]=false;
        const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,48000,96000,2,16,0};
        const auto sends=destinations(graph_.envelope[index]);
        const auto result=engine_->CreateSourceVoice(&graph_.voices[index],&format,0,4.0f,nullptr,&sends.list);
        error_=static_cast<std::uint32_t>(result);
        return SUCCEEDED(result) && configure_voice(index,voice_mix_[index]);
    }
    void close() override {
        if (engine_) engine_->UnregisterForCallbacks(this);
        graph_.clear(); envelopes_={};
        if (master_) { master_->DestroyVoice(); master_=nullptr; }
        if (engine_) { engine_->Release(); engine_=nullptr; }
        if (com_) { CoUninitialize(); com_=false; }
    }
    bool ready() const override { return master_!=nullptr; }
    unsigned voices() const override { return graph_.voices.front() ? 48U : 0U; }
    std::uint32_t error() const override { const auto critical=critical_error_.load(); return critical ? critical : error_; }
private:
    std::uint32_t operation_set() {
        const auto operation=next_operation_++;
        if (!next_operation_ || next_operation_==XAUDIO2_COMMIT_ALL) next_operation_=1;
        return operation;
    }
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
        std::array<IXAudio2SubmixVoice*,48> envelope{};
        std::array<IXAudio2SubmixVoice*,2> mix{},wet{};
        std::array<IXAudio2SourceVoice*,2> streams{};
        std::array<std::vector<std::int16_t>,2> pcm;
        std::array<std::vector<std::int16_t>,48> voice_pcm;
        struct QueuedPcm { std::vector<std::int16_t> pcm; std::uint64_t end; };
        std::array<std::deque<QueuedPcm>,48> voice_queue;
        std::array<std::uint64_t,48> voice_submitted{};
        std::array<bool,48> voice_loop{};
        ~Graph() { clear(); }
        void swap(Graph& other) { voices.swap(other.voices); envelope.swap(other.envelope); mix.swap(other.mix); wet.swap(other.wet); streams.swap(other.streams); pcm.swap(other.pcm); voice_pcm.swap(other.voice_pcm); voice_queue.swap(other.voice_queue); voice_submitted.swap(other.voice_submitted); voice_loop.swap(other.voice_loop); }
        void clear() {
            for (auto& stream:streams) if (stream) { stream->DestroyVoice(); stream=nullptr; }
            for (auto& samples:pcm) samples.clear();
            for (auto& voice:voices) if (voice) { voice->DestroyVoice(); voice=nullptr; }
            for (auto& bus:envelope) if (bus) { bus->DestroyVoice(); bus=nullptr; }
            for (auto& samples:voice_pcm) samples.clear();
            for (auto& samples:voice_queue) samples.clear();
            voice_submitted={};
            voice_loop={};
            for (unsigned core=0;core<2;++core) {
                if (wet[core]) { wet[core]->DestroyVoice(); wet[core]=nullptr; }
                if (mix[core]) { mix[core]->DestroyVoice(); mix[core]=nullptr; }
            }
        }
    };
    IXAudio2* engine_=nullptr;
    std::atomic<RevisionRequest*> revision_{nullptr};
    std::atomic<std::uint32_t> critical_error_{0};
    IXAudio2MasteringVoice* master_=nullptr;
    Graph graph_;
    std::array<CoreMix,2> core_mix_{};
    std::array<VoiceMix,48> voice_mix_{};
    std::array<std::shared_ptr<windows::EnvelopeObservation>,48> envelopes_;
    std::array<windows::EnvelopeParameters,48> envelope_parameters_{};
    std::uint32_t next_operation_=1;
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
