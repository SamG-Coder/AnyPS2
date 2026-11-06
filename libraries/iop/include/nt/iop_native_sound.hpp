#pragma once
#include <nt/audio.hpp>
#include <nt/audio_pitch.hpp>
#include <nt/adpcm.hpp>
#include <nt/adpcm_stream.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iop_native_heap.hpp>
#include <algorithm>
#include <chrono>
#include <bit>
#include <deque>

namespace nt::iop {
// The converter lowers validated LIBSD public entries to this native owner.
// Unimplemented sound operations stop before changing state or reporting success.
class NativeSoundServices final : public NativeServices,public NativeCallbackSource {
public:
    NativeSoundServices(NativeLibraryServices& libraries,audio::Output& output) : libraries_(libraries),output_(output) {}
    const char* transfer_failure() const { return transfer_failure_; }
    const auto& transfer_overlap() const { return transfer_overlap_; }
    const auto& transfer_arguments() const { return transfer_arguments_; }
    unsigned rejected_voice() const { return rejected_voice_; }
    audio::SampleError sample_error() const { return sample_error_; }
    std::span<const std::uint8_t> sample_storage() const { return samples_; }
    const auto& last_call() const { return last_call_; }
    std::uint64_t key_ons() const { return key_ons_; }
    std::uint64_t stream_submissions() const { return stream_submissions_; }
    std::uint64_t sample_bytes() const { return sample_bytes_; }
    std::uint32_t master(unsigned core,unsigned side) const { return cores_[core].master[side]; }
    std::uint32_t mix_flags(unsigned core) const { return cores_[core].mix; }
    bool stream_active(unsigned core) const { return streams_[core].active; }
    unsigned active_voices() const {
        unsigned count=0;
        for (const auto& voice:playback_) count+=voice.active;
        return count;
    }
    const auto& voice_volume(unsigned index) const { return voices_[index].volume; }
    bool core_output_enabled(unsigned core) const { return cores_[core].output_enabled; }
    // Sound settings played as the nearest native form; none ends the program.
    std::uint64_t approximated_settings() const { return approximated_settings_; }
    std::uint64_t stream_underruns() const { return stream_underruns_; }
    std::uint64_t voice_underruns() const { return voice_underruns_; }
    std::uint32_t stream_bytes() const { return stream_bytes_; }
    double longest_service_gap_ms() const { return longest_service_gap_ms_; }
    std::array<std::uint32_t,3> rejected_sample() const {
        const auto& voice=voices_[rejected_voice_];
        return {voice.start,voice.loop,voice.pitch};
    }
    void connect(NativeHeapServices& heap) { heap_=&heap; }
    std::chrono::steady_clock::duration voice_poll_interval=std::chrono::milliseconds(1);
    std::optional<NativeCallback> take() override {
        if (active_ || failure_) return {};
        // Each voice query takes the audio engine's lock, and this runs for every slice
        // of game code. Host playback advances asynchronously, so polling once a
        // millisecond observes the same progress; game reads still refresh directly.
        const auto now=std::chrono::steady_clock::now();
        if (next_voice_poll_!=std::chrono::steady_clock::time_point{})
            longest_service_gap_ms_=std::max(longest_service_gap_ms_,std::chrono::duration<double,std::milli>(now-next_voice_poll_).count());
        if (now>=next_voice_poll_) {
            next_voice_poll_=now+voice_poll_interval;
            for (unsigned index=0;index<playback_.size();++index) if (playback_[index].active && !refresh_voice(index)) {
                failure_=NativeResult{NativeStop::service_request,0,0,7}; return {};
            }
            for (unsigned core=0;core<2;++core) {
                auto& stream=streams_[core];
                if (!stream.active) continue;
                audio::StreamState state;
                if (!output_.stream_state(core,state)) { failure_=NativeResult{NativeStop::service_request,stream.pc,0,18}; return {}; }
                // Queue the other half of a looping stream before this one runs out,
                // while the program, signalled at the last boundary, has filled it.
                const auto half_frames=std::uint64_t(stream.size)/4;
                if (stream.loop && !stream.refill.address && !stream.next_queued && state.queued &&
                    stream.boundary-std::min(stream.boundary,state.frames)<=half_frames/2) {
                    if (!submit_stream(core,stream.half^1U,0)) { failure_=NativeResult{NativeStop::service_request,stream.pc,0,18}; return {}; }
                    stream.next_queued=true;
                }
                if (stream.loop && !stream.refill.address && state.queued && state.frames>=stream.boundary) {
                    // The audible position crossed into the queued half.
                    stream.half^=1; stream.start=0;
                    stream.origin=stream.boundary; stream.boundary+=half_frames; stream.next_queued=false;
                    notify(core);
                    continue;
                }
                if (state.queued) continue;
                ++stream_underruns_;
                if (stream.refill.address) {
                    store_native_word(arena_,scratch_,0); store_native_word(arena_,scratch_+4,0);
                    active_=true; active_stream_=core;
                    return NativeCallback{stream.refill.address,core,stream.refill.argument,stream.refill.gp,scratch_,scratch_+4};
                }
                if (stream.loop) {
                    // Drained before the host could queue ahead: restart from here.
                    if (stream.next_queued) { stream.half^=1; stream.next_queued=false; }
                    else {
                        stream.half^=1;
                        if (!submit_stream(core,stream.half,0)) { failure_=NativeResult{NativeStop::service_request,stream.pc,0,18}; return {}; }
                    }
                    stream.start=0;
                    stream.origin=state.frames; stream.boundary=state.frames+half_frames;
                } else stream.active=false;
                notify(core);
            }
        }
        if (callbacks_.empty()) return {};
        const auto callback=callbacks_.front(); callbacks_.pop_front(); active_=true;
        return callback;
    }
    void finish(std::uint32_t) override {
        active_=false;
        if (!active_stream_) return;
        const auto core=*active_stream_; active_stream_.reset(); auto& stream=streams_[core];
        if (!stream.active) return; // A callback may cancel its own stream.
        const auto pointer=load_native_word(arena_,scratch_),size=load_native_word(arena_,scratch_+4);
        if (!pointer && !size) { stream.active=false; return; }
        const auto address=ram_offset(pointer,1,arena_.size());
        if (!address || !size || size%1024 || size>arena_.size()-*address) {
            failure_=NativeResult{NativeStop::address_fault,stream.refill.address,pointer}; return;
        }
        stream.address=*address; stream.size=size;
        if (!submit_stream(core,stream.half,stream.start)) failure_=NativeResult{NativeStop::service_request,stream.pc,pointer,18};
    }
    std::optional<NativeResult> failure() const override { return failure_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::libsd) return {};
        last_call_={request.selector,request.arguments[0],request.arguments[1],request.arguments[2],request.arguments[3]};
        if (request.selector==0) {
            if (request.interrupt_context) return {NativeServiceStatus::unavailable};
            if (registered_) return {NativeServiceStatus::unavailable};
            const auto declaration=libraries_.declaration_for_entry("libsd",1,request.call_site);
            if (!declaration || !output_.open()) return {NativeServiceStatus::unavailable};
            const auto registered=libraries_.invoke({6,{*declaration},NativeServiceKind::loadcore},ram);
            if (registered.status!=NativeServiceStatus::returned || registered.value) {
                output_.close(); return {NativeServiceStatus::unavailable};
            }
            registered_=true;
            return {NativeServiceStatus::returned};
        }
        if (!registered_) return {NativeServiceStatus::unavailable};
        if (request.selector==13 || request.selector==14) {
            const auto& a=request.arguments;
            const auto value=request.selector==13 ? audio::note_to_pitch(static_cast<std::uint16_t>(a[0]),
                static_cast<std::uint16_t>(a[1]),static_cast<std::uint16_t>(a[2]),
                std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(a[3]))) :
                audio::pitch_to_note(static_cast<std::uint16_t>(a[0]),static_cast<std::uint16_t>(a[1]),static_cast<std::uint16_t>(a[2]));
            return value ? returned(*value) : unavailable();
        }
        if (request.selector==2) { output_.close(); initialized_=false; streams_={}; callbacks_.clear(); return returned(); }
        if (request.selector==4) {
            if (request.interrupt_context || request.arguments[0]>1 || !output_.initialize(request.arguments[0]!=0))
                return {NativeServiceStatus::unavailable};
            const auto old=cores_;
            cores_={}; voices_={}; playback_={}; handlers_={}; streams_={}; completed_={true,true}; callbacks_.clear(); failure_.reset(); arena_=ram;
            active_=false; active_stream_.reset(); digital_mode_=0;
            if (!request.arguments[0] || samples_.empty()) samples_.assign(sample_capacity,0);
            for (unsigned core=0;core<2;++core) {
                auto& c=cores_[core]; c.mix=core ? 0xffc : 0xff0;
                c.external={static_cast<std::uint16_t>(core ? 0x7fff : 0),static_cast<std::uint16_t>(core ? 0x7fff : 0)};
                c.effect_end=core ? 0x1fffff : 0x1dffff;
                if (request.arguments[0] && initialized_) { c.master=old[core].master; c.depth=old[core].depth; c.effect_end=old[core].effect_end; }
                c.effect[0]=core; c.effect_begin=(c.effect_end+1-16)&~1U;
                if (!apply_core(core,c)) return unavailable();
            }
            initialized_=true; return returned();
        }
        if (!initialized_) return unavailable();
        const auto& a=request.arguments;
        switch (request.selector) {
        case 5: case 6: return parameter(static_cast<std::uint16_t>(a[0]),static_cast<std::uint16_t>(a[1]),request.selector==5);
        case 7: case 8: return routing(static_cast<std::uint16_t>(a[0]),a[1],request.selector==7);
        case 9: case 10: return address(static_cast<std::uint16_t>(a[0]),a[1],request.selector==9);
        case 11: case 12: return core_attribute(static_cast<std::uint16_t>(a[0]),static_cast<std::uint16_t>(a[1]),request.selector==11);
        case 17: return voice_transfer(request,ram);
        case 18: return block_transfer(request,ram);
        case 19: return returned(completed_[a[0]&1] ? 1U : 0U);
        case 20: return stream_status(a[0]&1);
        case 23: case 24: return effect(a[0]&1U,a[1],request.selector==23,ram);
        case 25: return clear_effect(a[0]&1U,a[1],a[2]);
        case 26: case 27: return handler(request,ram);
        case 28: return returned(handlers_[a[0]&1].argument);
        case 29: return returned(handlers_[2].argument);
        case 30: return stop_stream(a[0]&1);
        default: return unavailable();
        }
    }
private:
    std::uint64_t approximated_settings_=0;
    std::uint64_t key_ons_=0,stream_submissions_=0,sample_bytes_=0,stream_underruns_=0,voice_underruns_=0;
    std::uint32_t stream_bytes_=0;
    double longest_service_gap_ms_=0;
    std::array<std::uint32_t,5> last_call_{};
    const char* transfer_failure_="none";
    std::array<std::uint32_t,5> transfer_arguments_{};
    std::array<std::uint32_t,3> transfer_overlap_{};
    static constexpr std::uint32_t sample_capacity=2*1024*1024;
    static constexpr std::array<std::uint32_t,10> effect_sizes{16,0x26c0,0x1f40,0x4840,0x6fe0,0xade0,0xf6c0,0x18040,0x18040,0x3c00};
    static std::optional<audio::ReverbSpace> effect_space(unsigned mode) {
        using audio::ReverbSpace;
        switch (mode) {
        case 0: return ReverbSpace::off;
        case 1: return ReverbSpace::room;
        case 2: return ReverbSpace::studio_small;
        case 3: return ReverbSpace::studio_medium;
        case 4: return ReverbSpace::studio_large;
        case 5: return ReverbSpace::hall;
        case 6: return ReverbSpace::space;
        case 9: return ReverbSpace::pipe;
        default: return {}; // Variable echo/delay still needs a native provider.
        }
    }
    struct Playback {
        bool active=false;
        std::optional<audio::AdpcmStream> stream;
        std::uint64_t observed_frames=0;
    };
    struct Voice {
        std::array<std::uint16_t,2> volume{},envelope{};
        std::uint16_t pitch=0x3fff;
        std::uint32_t start=0x5000,loop=0;
    };
    static audio::EnvelopeSpec envelope_spec(const Voice& voice) {
        const auto a=voice.envelope[0],b=voice.envelope[1];
        const auto curve=[](unsigned shift,int step,bool exponential) {
            return audio::GainCurve{32768U>>std::max(0,static_cast<int>(shift)-11),
                step*(1<<std::max(0,11-static_cast<int>(shift))),exponential};
        };
        const auto sustain_step=7-static_cast<int>((b>>6)&3);
        return {{{curve((a>>10)&31,7-((a>>8)&3),(a&0x8000)!=0),curve((a>>4)&15,-8,true),
            curve((b>>8)&31,(b&0x4000) ? -sustain_step-1 : sustain_step,(b&0x8000)!=0),
            curve(b&31,-8,(b&32)!=0)}},static_cast<std::uint32_t>((a&15)+1)<<11};
    }
    bool refresh_voice(unsigned index,bool refill=true) {
        auto& playback=playback_[index];
        if (!playback.active) return true;
        audio::StreamState stream; audio::EnvelopeState envelope;
        if (!output_.sample_state(index,stream)) return false;
        if (!stream.queued && playback.stream && !playback.stream->finished()) ++voice_underruns_;
        if (playback.stream) {
            auto& decoder=*playback.stream;
            for (const auto& block:decoder.pending()) {
                if (block.loop_start && block.begin>=playback.observed_frames && block.begin<stream.frames)
                    voices_[index].loop=block.address;
                if (block.end && block.begin+28>playback.observed_frames && block.begin+28<=stream.frames)
                    cores_[index/24].end_mask|=1U<<(index%24);
                if (block.begin>=playback.observed_frames && block.begin<stream.frames)
                    for (unsigned core=0;core<2;++core)
                        if (cores_[core].irq_enabled && cores_[core].interrupt_address==block.address && handlers_[2].address) {
                            if (callbacks_.size()>=1024) return false;
                            callbacks_.push_back({handlers_[2].address,1U<<core,handlers_[2].argument,handlers_[2].gp});
                        }
            }
            playback.observed_frames=stream.frames; decoder.retire(stream.frames);
            const auto target=stream.frames+stream_blocks(voices_[index])*28;
            while (refill && !decoder.finished() && decoder.frames()+8*28<=target) {
                auto next=decoder;
                std::vector<std::int16_t> pcm;
                const auto result=next.decode(samples_,8,pcm);
                if (result!=audio::SampleError::none) { rejected_voice_=index; sample_error_=result; return false; }
                if (!output_.append_voice(index,pcm)) return false;
                decoder=std::move(next);
            }
            if (!output_.sample_state(index,stream)) return false;
        }
        if (!output_.envelope_state(index,envelope)) return false;
        if (!stream.queued || !envelope.active) {
            if (stream.queued && !output_.stop_voice(index)) return false;
            playback.active=false;
        }
        return true;
    }
    static unsigned stream_blocks(const Voice& voice) {
        // About 75 ms of source PCM at the current pitch, bounded independently
        // of program polling frequency, which covers the pauses between services
        // while a frame finishes. Writes over queued PCM revise it, so this adds no
        // delay to key-on, key-off or volume. The audio engine owns elapsed frames.
        return std::clamp((128U*voice.pitch+4095)/4096,32U,224U);
    }
    NativeServiceReply key_on(unsigned core,std::uint32_t mask) {
        key_ons_+=std::popcount(mask);
        std::array<std::vector<std::int16_t>,24> decoded;
        std::array<std::optional<audio::AdpcmStream>,24> streams;
        std::vector<audio::VoiceSample> starts;
        sample_error_=audio::SampleError::none;
        for (unsigned voice=0;voice<24;++voice) if (mask&(1U<<voice)) {
            const auto index=core*24+voice; const auto& v=voices_[index];
            streams[voice].emplace(v.start,v.loop);
            const auto error=streams[voice]->decode(samples_,std::min(stream_blocks(v),128U),decoded[voice]);
            if (error!=audio::SampleError::none) { rejected_voice_=index; sample_error_=error; return unavailable(); }
            starts.push_back({index,decoded[voice],0,0,envelope_spec(v)});
        }
        if (!output_.start_voices(starts)) return unavailable();
        for (const auto& start:starts) {
            playback_[start.voice]={true,std::move(streams[start.voice%24]),0};
        }
        cores_[core].end_mask&=~mask;
        return returned();
    }
    struct Core {
        std::array<std::uint16_t,2> master{},depth{},external{},input{};
        std::array<std::uint32_t,4> route{0xffffff,0xffffff,0xffffff,0xffffff};
        std::uint16_t mix=0;
        std::uint32_t effect_end=0,effect_begin=0;
        std::array<std::uint32_t,5> effect{};
        std::uint32_t transfer_address=0,interrupt_address=0,end_mask=0;
        std::uint16_t noise_clock=0;
        bool effect_enabled=false,irq_enabled=false;
        // The attribute LIBSD names mute enable is the hardware bit where 1 passes
        // the output and 0 silences it.
        bool output_enabled=true;
    };
    struct Handler { std::uint32_t address=0,argument=0,gp=0; };
    struct Stream { bool active=false,loop=false; unsigned half=0; std::uint32_t address=0,size=0,pc=0,start=0; std::uint64_t origin=0,boundary=0; bool next_queued=false; Handler refill; };
    static NativeServiceReply returned(std::uint32_t value=0) { return {NativeServiceStatus::returned,value}; }
    static NativeServiceReply unavailable() { return {NativeServiceStatus::unavailable}; }
    static float fixed_gain(std::uint16_t value) {
        const auto signed_value=(value&0x4000) ? static_cast<int>(value&0x7fff)-0x8000 : static_cast<int>(value&0x7fff);
        return static_cast<float>(signed_value)/16384.0f;
    }
    static float linear_gain(std::uint16_t value) { return static_cast<float>(std::bit_cast<std::int16_t>(value))/32768.0f; }
    static audio::StereoGain gated(const std::array<std::uint16_t,2>& value,std::uint16_t mask,unsigned bit) {
        return {(mask&(1U<<(bit+1))) ? linear_gain(value[0]) : 0,(mask&(1U<<bit)) ? linear_gain(value[1]) : 0};
    }
    bool apply_voice(unsigned index,const Voice& v,const Core& c) {
        const auto bit=1U<<(index%24);
        audio::VoiceMix mix;
        mix.direct={(c.route[0]&bit) && (c.mix&0x800) ? fixed_gain(v.volume[0]) : 0,
                    (c.route[2]&bit) && (c.mix&0x400) ? fixed_gain(v.volume[1]) : 0};
        mix.effect={(c.route[1]&bit) && (c.mix&0x200) ? fixed_gain(v.volume[0]) : 0,
                    (c.route[3]&bit) && (c.mix&0x100) ? fixed_gain(v.volume[1]) : 0};
        mix.rate=static_cast<float>(v.pitch)/4096.0f;
        return output_.configure_voice(index,mix);
    }
    bool apply_core(unsigned core,const Core& c) {
        const auto space=effect_space(c.effect[1]&~0x100U);
        if (!space) return false;
        audio::CoreMix mix;
        mix.output={fixed_gain(c.master[0]),fixed_gain(c.master[1])};
        if (!c.output_enabled) mix.output={};
        mix.external_direct=gated(c.external,c.mix,2); mix.external_effect=gated(c.external,c.mix,0);
        mix.input_direct=gated(c.input,c.mix,6); mix.input_effect=gated(c.input,c.mix,4);
        mix.reverb={*space,{linear_gain(c.depth[0]),linear_gain(c.depth[1])},c.effect_enabled};
        if (!output_.configure_core(core,mix)) return false;
        for (unsigned v=core*24;v<(core+1)*24;++v) if (!apply_voice(v,voices_[v],c)) return false;
        return true;
    }
    // Bit 15 selects a sweep; bit 13 selects a falling one.
    std::uint16_t settled_gain(std::uint16_t value) {
        if (!(value&0x8000)) return value;
        ++approximated_settings_;
        return (value&0x2000) ? 0 : 0x3fff;
    }
    NativeServiceReply parameter(std::uint16_t entry,std::uint16_t value,bool writing) {
        const unsigned core=entry&1U,kind=entry>>8,voice=(entry&0x3e)>>1;
        if (kind<=7) {
            if ((entry&0xc0) || voice>=24) return unavailable();
            const auto index=core*24+voice; auto v=voices_[index];
            std::uint16_t* field=nullptr;
            if (kind<2) field=&v.volume[kind];
            else if (kind==2) field=&v.pitch;
            else if (kind<5) field=&v.envelope[kind-3];
            else if (!writing) {
                if (kind!=5) return returned(static_cast<std::uint16_t>(v.volume[kind-6]<<1));
                audio::EnvelopeState envelope;
                if (!refresh_voice(index) || !output_.envelope_state(index,envelope)) return unavailable();
                return returned(envelope.level);
            }
            else return unavailable();
            if (!writing) return returned(*field);
            // A gain sweep has no timed native envelope: go straight to where it ends.
            if (kind<2) value=settled_gain(value);
            // Keep pitch inside the native resampler's range.
            if (kind==2 && (value>0x3fff || (value>0 && value<4))) {
                ++approximated_settings_;
                value=value>0x3fff ? 0x3fff : 4;
            }
            *field=value;
            if (kind>=3 && !output_.configure_envelope(index,envelope_spec(v))) return unavailable();
            if (!apply_voice(index,v,cores_[core])) return unavailable();
            voices_[index]=v; return returned();
        }
        if ((entry&0xfe)!=(kind==8 ? 0 : 0x80)) return unavailable();
        auto c=cores_[core]; std::uint16_t* field=nullptr;
        if (kind==8) field=&c.mix;
        else if (kind<=10) field=&c.master[kind-9];
        else if (kind<=12) field=&c.depth[kind-11];
        else if (kind<=14) field=&c.external[kind-13];
        else if (kind<=16) field=&c.input[kind-15];
        else if (kind<=18 && !writing) return returned(static_cast<std::uint16_t>(c.master[kind-17]<<1));
        else return unavailable();
        if (!writing) return returned(*field);
        if (kind==9 || kind==10) value=settled_gain(value);
        if (kind==8 && value>0xfff) return unavailable();
        *field=value;
        if (!apply_core(core,c)) return unavailable();
        cores_[core]=c; return returned();
    }
    NativeServiceReply routing(std::uint16_t entry,std::uint32_t value,bool writing) {
        if (entry&0xfe) return unavailable();
        const unsigned core=entry&1U,kind=entry>>8; value&=0xffffff;
        // Pitch modulation and noise have no native mixer stage. Voices keep their
        // own pitch and samples, so such a sound differs, but the program continues.
        if (kind==0x13 || kind==0x14) {
            if (writing && value) ++approximated_settings_;
            return returned();
        }
        if (kind==0x17) {
            for (unsigned index=core*24;index<(core+1)*24;++index) if (!refresh_voice(index)) return unavailable();
            auto& ended=cores_[core].end_mask;
            if (writing) ended=0; // The public clear operation acknowledges latched endings.
            return returned(ended);
        }
        if (kind==0x16 && writing) {
            for (unsigned voice=0;voice<24;++voice)
                if ((value&(1U<<voice)) && playback_[core*24+voice].active && !output_.release_voice(core*24+voice)) return unavailable();
            return returned();
        }
        if (kind==0x15 && writing) return value ? key_on(core,value) : returned();
        if (kind<0x18 || kind>0x1b) return unavailable();
        auto c=cores_[core]; auto& field=c.route[kind-0x18];
        if (!writing) return returned(field);
        field=value;
        if (!apply_core(core,c)) return unavailable();
        cores_[core]=c; return returned();
    }
    NativeServiceReply address(std::uint16_t entry,std::uint32_t value,bool writing) {
        const unsigned core=entry&1U,kind=entry>>8,voice=(entry&0x3e)>>1;
        // The address interface selects core/voice independently of bits 6-7.
        if (kind<0x1c || kind>0x22 || (kind<0x20 && voice) || voice>=24) return unavailable();
        if (kind==0x22) {
            const auto index=core*24+voice;
            if (writing || !refresh_voice(index) || !playback_[index].stream) return unavailable();
            return returned(playback_[index].stream->source_position(playback_[index].observed_frames));
        }
        if (!writing && kind==0x21 && !refresh_voice(core*24+voice)) return unavailable();
        std::uint32_t* field=nullptr;
        if (kind==0x1d) field=&cores_[core].effect_end;
        else if (kind==0x1c) field=&cores_[core].effect_begin;
        else if (kind==0x1e) field=&cores_[core].transfer_address;
        else if (kind==0x1f) field=&cores_[core].interrupt_address;
        else if (kind==0x20) field=&voices_[core*24+voice].start;
        else if (kind==0x21) field=&voices_[core*24+voice].loop;
        else return unavailable();
        if (!writing) return returned(*field);
        // A voice already playing keeps the loop point it started with; the new one
        // applies from its next start. Limitation: a program that redirects a loop
        // while the voice plays hears the old loop until then.
        if (value>=sample_capacity) return unavailable();
        *field=kind==0x1d ? value|0x1ffffU : value&~15U;
        return returned();
    }
    NativeServiceReply core_attribute(std::uint16_t entry,std::uint16_t value,bool writing) {
        const unsigned core=entry&1,kind=entry&~1U;
        auto c=cores_[core];
        if (kind==8) {
            if (writing) cores_[core].noise_clock=value&63;
            return returned(cores_[core].noise_clock);
        }
        if (kind==10) {
            if (!writing) return returned(digital_mode_);
            // Host PCM output can preserve the PCM path only. Encoded digital
            // bypass modes require a native encoded-audio backend.
            if ((value&15)!=0 || (value&~0x0c80U)) return unavailable();
            digital_mode_=value; return returned();
        }
        bool* field=kind==2 ? &c.effect_enabled : kind==4 ? &c.irq_enabled : kind==6 ? &c.output_enabled : nullptr;
        if (!field) return unavailable();
        if (!writing) return returned(*field);
        *field=(value&1)!=0;
        if (!apply_core(core,c)) return unavailable();
        cores_[core]=c; return returned();
    }
    void notify(unsigned core) {
        const auto& handler=handlers_[core];
        if (handler.address) callbacks_.push_back({handler.address,core,handler.argument,handler.gp});
    }
    bool write_samples(std::uint32_t destination,std::span<const std::uint8_t> bytes) {
        sample_bytes_+=bytes.size();
        auto target=std::span(samples_).subspan(destination,bytes.size());
        if (std::equal(bytes.begin(),bytes.end(),target.begin())) return true;
        std::vector<unsigned> affected;
        for (unsigned index=0;index<playback_.size();++index) {
            // Observe consumption before replacing data, but do not queue more
            // PCM from the old bytes immediately before this refill.
            if (!refresh_voice(index,false)) { transfer_failure_="host-voice-state"; return false; }
            const auto& playback=playback_[index];
            if (playback.active && playback.stream && playback.stream->overlaps(destination,static_cast<unsigned>(bytes.size())))
                affected.push_back(index);
        }
        if (affected.empty()) { std::copy(bytes.begin(),bytes.end(),target.begin()); return true; }
        struct Restore {
            std::span<std::uint8_t> target;
            std::vector<std::uint8_t> old;
            bool committed=false;
            ~Restore() { if (!committed) std::copy(old.begin(),old.end(),target.begin()); }
        } restore{target,{target.begin(),target.end()}};
        std::copy(bytes.begin(),bytes.end(),target.begin());
        std::array<std::optional<audio::AdpcmStream>,48> revised;
        bool decode_failed=false;
        const auto accepted=output_.revise_voices([&](std::span<const audio::StreamState> states,std::vector<audio::VoiceRevision>& edits) {
            if (states.size()!=playback_.size()) return false;
            for (const auto index:affected) {
                auto& decoder=revised[index]; decoder=playback_[index].stream;
                audio::VoiceRevision edit; edit.voice=index;
                const auto result=decoder->revise(samples_,states[index].frames,destination,
                    static_cast<unsigned>(bytes.size()),edit.begin,edit.pcm);
                if (result!=audio::SampleError::none) {
                    rejected_voice_=index; sample_error_=result;
                    decode_failed=true;
                    transfer_failure_="stream-revision-decode"; return false;
                }
                if (!edit.pcm.empty()) edits.push_back(std::move(edit));
            }
            return true;
        });
        if (!accepted) {
            if (!decode_failed) transfer_failure_="host-stream-revision";
            return false;
        }
        for (const auto index:affected) playback_[index].stream=std::move(revised[index]);
        restore.committed=true; return true;
    }
    NativeServiceReply voice_transfer(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments; const unsigned core=a[0]&1,mode=a[1]&0xffff;
        transfer_failure_="none"; transfer_overlap_={};
        transfer_arguments_={a[0],a[1],a[2],a[3],0};
        if (mode&~9U) { transfer_failure_="unsupported-mode"; return unavailable(); }
        if (streams_[core].active) { transfer_failure_="block-stream-active"; return unavailable(); }
        if (callbacks_.size()>=1024) { transfer_failure_="callback-capacity"; return unavailable(); }
        if (request.stack_pointer>0xffffffebU) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
        if (!stack) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto requested=load_native_word(ram,*stack);
        transfer_arguments_[4]=requested;
        if (requested>sample_capacity) return returned(0xffffffff);
        const auto alignment=(mode&8) ? 2U : 64U;
        const auto count=(requested+alignment-1)&~(alignment-1),destination=a[3]&~1U;
        const auto source=ram_offset(a[2],1,ram.size());
        if (!source || count>ram.size()-*source) return {NativeServiceStatus::address_fault,0,a[2]};
        if (destination>samples_.size() || count>samples_.size()-destination) return returned(0xffffffff);
        if (mode&1) {
            for (auto word=*source&~3U;word<*source+count;word+=4)
                if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,a[2]};
            std::copy_n(samples_.begin()+destination,count,ram.begin()+*source);
        } else {
            if (!write_samples(destination,ram.subspan(*source,count))) return unavailable();
        }
        cores_[core].transfer_address=destination+count; completed_[core]=true;
        // The direct native copy has finished. Defer the compiled interrupt
        // callback to the scheduler; polling observes the completed copy.
        if (!(mode&8)) notify(core);
        if (count && cores_[core].irq_enabled && cores_[core].interrupt_address>=destination &&
            cores_[core].interrupt_address<destination+count && handlers_[2].address)
            callbacks_.push_back({handlers_[2].address,1U<<core,handlers_[2].argument,handlers_[2].gp});
        return returned((mode&8) ? 0U : requested);
    }
    bool submit_stream(unsigned core,unsigned half,std::uint32_t skip) {
        ++stream_submissions_;
        stream_bytes_=streams_[core].size;
        auto& stream=streams_[core];
        // A stream started part way through a buffer plays only its remainder once.
        const auto address=stream.address+half*stream.size+skip;
        const auto bytes=stream.size-skip;
        if (address>arena_.size() || bytes>arena_.size()-address || !bytes || stream.size%1024) return false;
        std::vector<std::int16_t> pcm(bytes/2);
        // LIBSD input buffers store 256 left samples followed by 256 right
        // samples in each 1024-byte block. XAudio2 consumes interleaved stereo.
        for (unsigned block=0;block<bytes;block+=1024)
            for (unsigned frame=0;frame<256;++frame)
                for (unsigned channel=0;channel<2;++channel) {
                    const auto at=address+block+channel*512+frame*2;
                    const auto word=std::uint16_t(arena_[at]|(unsigned(arena_[at+1])<<8));
                    pcm[block/2+frame*2+channel]=std::bit_cast<std::int16_t>(word);
                }
        audio::StreamState state;
        if (!output_.stream_state(core,state)) return false;
        // A queued half keeps the position of the half still playing.
        if (!stream.next_queued && !stream.boundary) { stream.origin=state.frames; stream.boundary=state.frames+bytes/4; }
        else if (!stream.loop || stream.refill.address) { stream.origin=state.frames; stream.boundary=state.frames+bytes/4; }
        return output_.submit_stream(core,pcm);
    }
    NativeServiceReply stream_status(unsigned core) {
        const auto& stream=streams_[core];
        if (!stream.active) return returned();
        audio::StreamState state;
        if (!output_.stream_state(core,state)) return unavailable();
        const auto consumed=state.frames>=stream.origin ? state.frames-stream.origin : 0;
        const auto offset=std::min<std::uint64_t>(consumed*4+stream.start,stream.size);
        return returned((stream.half<<24)|((stream.address+stream.half*stream.size+static_cast<unsigned>(offset))&0xffffff));
    }
    NativeServiceReply stop_stream(unsigned core) {
        const auto state=stream_status(core);
        if (state.status!=NativeServiceStatus::returned || !output_.stop_stream(core)) return unavailable();
        streams_[core]={}; return state;
    }
    NativeServiceReply block_transfer(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments; const unsigned core=a[0]&1,mode=a[1]&0xffff;
        if (mode==2) return stop_stream(core);
        // Mode bit 0 and 1 together select starting at a fifth, stacked address.
        const bool from_address=(mode&3)==3;
        if (mode!=0 && mode!=0x10 && mode!=0x80 && mode!=3 && mode!=0x13) return unavailable();
        if (streams_[core].active) return returned(0xffffffff);
        const auto source=ram_offset(a[2],1,ram.size());
        if (!source || a[3]>ram.size()-*source) return {NativeServiceStatus::address_fault,0,a[2]};
        const bool loop=(mode&0x10)!=0;
        const auto size=loop ? a[3]/2 : a[3];
        if (!size || size%1024 || (loop && a[3]%2)) return returned(0xffffffff);
        unsigned half=0;
        std::uint32_t start=0;
        if (from_address) {
            if (request.stack_pointer>0xffffffebU) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
            if (!stack) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto first=load_native_word(ram,*stack);
            const auto position=first-a[2];
            // Blocks hold separate channels, so only whole blocks can be skipped.
            if (position>=a[3] || position%1024) return unavailable();
            half=position/size; start=position%size;
        }
        Handler refill;
        if (mode==0x80) {
            if (request.stack_pointer>0xffffffe7U) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
            if (!stack || 8>ram.size()-*stack) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
            refill={load_native_word(ram,*stack),load_native_word(ram,*stack+4),request.global_pointer};
            const auto target=ram_offset(refill.address,4,ram.size());
            if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,refill.address};
            if (!scratch_) {
                if (!heap_) return unavailable();
                const auto allocated=heap_->invoke({4,{0,8},NativeServiceKind::sysmem},ram);
                if (allocated.status!=NativeServiceStatus::returned || !allocated.value) return unavailable();
                scratch_=allocated.value;
                if (scratch_>ram.size() || 8>ram.size()-scratch_ || nt_iop_native_code_word(scratch_) || nt_iop_native_code_word(scratch_+4))
                    return {NativeServiceStatus::code_write,0,scratch_};
            }
        }
        streams_[core]={true,loop,half,*source,size,request.call_site};
        streams_[core].start=start;
        streams_[core].refill=refill;
        if (!submit_stream(core,half,start)) { streams_[core]={}; return unavailable(); }
        return returned();
    }
    NativeServiceReply clear_effect(unsigned core,std::uint32_t channel,std::uint32_t mode) {
        // Byte capacities specified by LIBSD's public effect workspace ABI.
        if (mode>=effect_sizes.size() || channel>1) return returned(0xffffffffU);
        if (!mode) return returned();
        const auto end=cores_[core].effect_end+1;
        if (end>samples_.size() || effect_sizes[mode]>end) return unavailable();
        if (mode==(cores_[core].effect[1]&~0x100U) && !output_.clear_reverb(core)) return unavailable();
        std::fill(samples_.begin()+end-effect_sizes[mode],samples_.begin()+end,0);
        return returned();
    }
    NativeServiceReply effect(unsigned core,std::uint32_t pointer,bool writing,std::span<std::uint8_t> ram) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || 20>ram.size()-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        if (!writing) {
            for (unsigned i=0;i<20;i+=4) if (nt_iop_native_code_word(*offset+i)) return {NativeServiceStatus::code_write,0,pointer};
            for (unsigned i=0;i<5;++i) store_native_word(ram,*offset+i*4,cores_[core].effect[i]);
            return returned();
        }
        std::array<std::uint32_t,5> config{};
        for (unsigned i=0;i<5;++i) config[i]=load_native_word(ram,*offset+i*4);
        const auto mode=config[1]&~0x100U;
        if (mode>=effect_sizes.size()) return returned(0xffffffffU);
        if (!effect_space(mode)) return unavailable();
        config[0]=core; config[3]=0; config[4]=0;
        auto c=cores_[core]; const auto old_mode=c.effect[1]&~0x100U;
        const auto end=c.effect_end+1;
        const auto clear_size=std::max(effect_sizes[mode],c.effect_enabled ? effect_sizes[old_mode] : 0U);
        if (end>samples_.size() || clear_size>end) return unavailable();
        c.effect=config; c.depth={static_cast<std::uint16_t>(config[2]),static_cast<std::uint16_t>(config[2]>>16)};
        c.effect_begin=(end-effect_sizes[mode])&~1U;
        if (!apply_core(core,c)) return unavailable();
        if (config[1]&0x100U) {
            if (!output_.clear_reverb(core)) return unavailable();
            std::fill(samples_.begin()+end-clear_size,samples_.begin()+end,0);
        }
        cores_[core]=c;
        return returned();
    }
    NativeServiceReply handler(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const bool transfer=request.selector==26;
        const auto& a=request.arguments;
        if (transfer && a[0]>1) return unavailable();
        const auto slot=transfer ? a[0] : 2U;
        const auto target=a[transfer ? 1 : 0];
        if (target) {
            const auto offset=ram_offset(target,4,ram.size());
            if (!offset || !nt_iop_native_instruction(*offset)) return {NativeServiceStatus::unmapped_code,0,target};
        }
        const auto previous=handlers_[slot].address;
        handlers_[slot]={target,a[transfer ? 2 : 1],request.global_pointer};
        return returned(previous);
    }
    NativeLibraryServices& libraries_;
    audio::Output& output_;
    std::array<Voice,48> voices_{};
    std::array<Core,2> cores_{};
    std::array<Handler,3> handlers_{};
    std::vector<std::uint8_t> samples_;
    std::array<Playback,48> playback_{};
    std::chrono::steady_clock::time_point next_voice_poll_{};
    unsigned rejected_voice_=0;
    audio::SampleError sample_error_=audio::SampleError::none;
    std::array<Stream,2> streams_{};
    std::array<bool,2> completed_{true,true};
    std::deque<NativeCallback> callbacks_;
    std::optional<NativeResult> failure_;
    std::span<std::uint8_t> arena_;
    std::uint16_t digital_mode_=0;
    bool active_=false;
    NativeHeapServices* heap_=nullptr;
    std::uint32_t scratch_=0;
    std::optional<unsigned> active_stream_;
    bool registered_=false;
    bool initialized_=false;
};
}
