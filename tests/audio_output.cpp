#include <nt/audio.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <array>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include "../libraries/audio/src/windows_reverb.hpp"
#include "../libraries/audio/src/windows_envelope.hpp"
namespace {
void verify_stationary_envelope() {
    using namespace nt::audio;
    auto observation=std::make_shared<windows::EnvelopeObservation>();
    Microsoft::WRL::ComPtr<IXAPO> processor;
    processor.Attach(new windows::EnvelopeEffect(observation));
    Microsoft::WRL::ComPtr<IXAPOParameters> controls;
    if (FAILED(processor.As(&controls))) throw std::runtime_error("Envelope controls unavailable");
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,1,48000,192000,4,32,0};
    XAPO_LOCKFORPROCESS_PARAMETERS locked{&format,64};
    if (FAILED(processor->LockForProcess(1,&locked,1,&locked))) throw std::runtime_error("Envelope format rejected");
    windows::EnvelopeParameters parameters;
    parameters.active=true; parameters.spec.curves[0]={32768,64,false};
    parameters.spec.curves[3]={32768,-1024,false};
    std::array<float,64> input,output{}; input.fill(0.25f);
    const auto process=[&](bool silent) {
        controls->SetParameters(&parameters,sizeof(parameters));
        XAPO_PROCESS_BUFFER_PARAMETERS source{input.data(),silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID,64};
        XAPO_PROCESS_BUFFER_PARAMETERS destination{output.data(),XAPO_BUFFER_VALID,64};
        processor->Process(1,&source,1,&destination,TRUE);
        return destination.BufferFlags;
    };
    process(false); const auto before=output.back();
    parameters.hold=true; input.fill(-1);
    if (process(true)!=XAPO_BUFFER_VALID || output.front()<=before || output.back()<=output.front())
        throw std::runtime_error("Stationary source lost its held sample or advancing envelope");
    parameters.hold=false; process(false);
    if (output.front()>=0) throw std::runtime_error("Resumed source retained its held sample");
    parameters.hold=true; parameters.release=true; process(true);
    if (output.back()!=0 || (observation->value.load()&0x10000))
        throw std::runtime_error("Stationary source release failed to reach silence");
    processor->UnlockForProcess();
}
void verify_reverb(nt::audio::ReverbSpace space) {
    Microsoft::WRL::ComPtr<IUnknown> effect;
    Microsoft::WRL::ComPtr<IXAPO> processor;
    Microsoft::WRL::ComPtr<IXAPOParameters> controls;
    XAUDIO2FX_REVERB_PARAMETERS parameters{};
    if (!nt::audio::windows::reverb_parameters(space,parameters) ||
        FAILED(XAudio2CreateReverb(effect.GetAddressOf())) || FAILED(effect.As(&processor)) || FAILED(effect.As(&controls)))
        throw std::runtime_error("Native reverb effect creation failed");
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
    XAPO_LOCKFORPROCESS_PARAMETERS locked{&format,256};
    if (FAILED(processor->LockForProcess(1,&locked,1,&locked)))
        throw std::runtime_error("Native reverb rejected stereo float processing");
    controls->SetParameters(&parameters,sizeof(parameters));
    std::array<float,512> input{},output{};
    double tail=0;
    for (unsigned block=0;block<400;++block) {
        input.fill(0); output.fill(0);
        if (!block) { input[0]=0.5f; input[1]=0.25f; }
        XAPO_PROCESS_BUFFER_PARAMETERS source{input.data(),XAPO_BUFFER_VALID,256};
        XAPO_PROCESS_BUFFER_PARAMETERS destination{output.data(),XAPO_BUFFER_VALID,256};
        processor->Process(1,&source,1,&destination,TRUE);
        for (const auto sample:output) {
            if (!std::isfinite(sample)) throw std::runtime_error("Native reverb produced nonfinite audio");
            if (block>=4) tail+=double(sample)*sample;
        }
    }
    processor->Reset();
    input.fill(0); output.fill(1);
    XAPO_PROCESS_BUFFER_PARAMETERS source{input.data(),XAPO_BUFFER_VALID,256};
    XAPO_PROCESS_BUFFER_PARAMETERS destination{output.data(),XAPO_BUFFER_VALID,256};
    processor->Process(1,&source,1,&destination,TRUE);
    double cleared=0;
    for (const auto sample:output) cleared+=double(sample)*sample;
    processor->UnlockForProcess();
    if (tail<=1e-10 || cleared>1e-12)
        throw std::runtime_error("Native reverb did not produce a tail or clear its history");
}
}
#endif

int main() {
    try {
        auto output=nt::audio::make_output();
        if (output->ready() || output->voices()) throw std::runtime_error("Output started without initialization");
        if (!output->open() || !output->ready()) throw std::runtime_error("Cannot open the native output device");
#ifdef _WIN32
        verify_stationary_envelope();
        for (const auto space:{nt::audio::ReverbSpace::room,nt::audio::ReverbSpace::studio_small,
                              nt::audio::ReverbSpace::studio_medium,nt::audio::ReverbSpace::studio_large,
                              nt::audio::ReverbSpace::hall,nt::audio::ReverbSpace::space,nt::audio::ReverbSpace::pipe})
            verify_reverb(space);
#endif
        for (bool preserve : {false,true})
            if (!output->initialize(preserve) || output->voices()!=48 || output->error())
                throw std::runtime_error("Native source-voice initialization failed");
        nt::audio::CoreMix core;
        core.output={0.5f,-1.0f}; core.external_direct={0.25f,0.75f};
        for (unsigned c=0;c<2;++c)
            if (!output->configure_core(c,core)) throw std::runtime_error("Native stereo bus configuration failed");
        core.reverb={nt::audio::ReverbSpace::studio_large,{0.25f,-0.5f},true};
        for (unsigned c=0;c<2;++c) {
            if (!output->configure_core(c,core) || !output->clear_reverb(c))
                throw std::runtime_error("Native reverb bus activation or replacement failed");
        }
        core.reverb.enabled=false;
        if (!output->configure_core(0,core) || output->clear_reverb(2))
            throw std::runtime_error("Native reverb disable or invalid core handling failed");
        for (unsigned voice=0;voice<48;++voice) {
            if (!output->configure_voice(voice,{{0.5f,-0.5f},{0,0},0.5f}) ||
                !output->configure_voice(voice,{{0,0},{0,0},0}))
                throw std::runtime_error("Native voice gain, routing or zero-rate suspension failed");
        }
        if (output->configure_voice(48,{}) || output->configure_core(2,{}))
            throw std::runtime_error("Invalid native sound channel accepted");
        std::vector<std::int16_t> silence(9600);
        if (!output->submit_stream(0,silence)) throw std::runtime_error("Native PCM submission failed");
        nt::audio::StreamState state;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do {
            if (!output->stream_state(0,state)) throw std::runtime_error("Native PCM state unavailable");
            if (!state.queued) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now()<deadline);
        if (state.queued || state.frames<4800 || !output->stop_stream(0)) throw std::runtime_error("Native PCM did not finish consumption");
        // Silence exercises real device consumption without making test noise.
        std::vector<std::int16_t> mono(480);
        if (!output->configure_voice(24,{{0,0},{},1}) || !output->submit_sample(24,mono,0,480))
            throw std::runtime_error("Native looping sample submission failed");
        mono.assign(480,123); // The engine must own its own copy.
        const auto sample_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do {
            if (!output->sample_state(24,state)) throw std::runtime_error("Native voice state unavailable");
            if (state.frames>=2400) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now()<sample_deadline);
        if (!state.queued || state.frames<2400 || output->submit_sample(24,mono,0,0) || !output->stop_voice(24) ||
            !output->sample_state(24,state) || state.queued || state.frames)
            throw std::runtime_error("Native sample loop, busy rejection or stop ownership failed");
        mono.assign(480,0);
        if (output->submit_sample(24,mono,470,20) || !output->submit_sample(24,mono,0,0))
            throw std::runtime_error("Native sample replay or loop bounds failed");
        if (!output->stop_voice(24)) throw std::runtime_error("Native sample cleanup failed");
        const nt::audio::VoiceSample streaming{25,mono,0,0,{}};
        if (!output->start_voices(std::span(&streaming,1)) || !output->append_voice(25,mono) ||
            !output->append_voice(25,mono) || !output->configure_voice(25,{{0,0},{},1}))
            throw std::runtime_error("Native queued voice initialization failed");
        mono.assign(480,123); // All three submitted buffers must remain owned.
        const auto queued_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do {
            if (!output->sample_state(25,state)) throw std::runtime_error("Native queued cursor unavailable");
            if (!state.queued) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now()<queued_deadline);
        if (state.queued || state.frames!=1440 || !output->stop_voice(25))
            throw std::runtime_error("Native appended PCM lost its cumulative cursor or buffer ownership");
        // Real device clock: zero pitch holds the source cursor while the
        // downstream envelope advances, releases and survives pitch resumption.
        mono.assign(48000,4096);
        nt::audio::EnvelopeSpec slow;
        slow.curves[0]={32768,1,false}; slow.curves[3]={32768,-128,false};
        const nt::audio::VoiceSample held{26,mono,0,0,slow};
        if (!output->configure_voice(26,{{0,0},{},0}) || !output->start_voices(std::span(&held,1)))
            throw std::runtime_error("Zero-pitch key-on failed");
        const auto wait_for=[&](auto condition) {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            do {
                if (condition()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now()<end);
            throw std::runtime_error("Stationary-source audio observation timed out");
        };
        nt::audio::EnvelopeState gain;
        wait_for([&] { return output->envelope_state(26,gain) && gain.level>=480; });
        if (!output->sample_state(26,state) || state.frames!=0 || !state.queued)
            throw std::runtime_error("Zero pitch consumed source samples");
        if (!output->configure_voice(26,{{0,0},{},1})) throw std::runtime_error("Pitch resume failed");
        wait_for([&] { return output->sample_state(26,state) && state.frames>=960; });
        if (!output->configure_voice(26,{{0,0},{},0})) throw std::runtime_error("Active zero-pitch change failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        output->sample_state(26,state); output->envelope_state(26,gain);
        const auto held_position=state.frames; const auto held_level=gain.level;
        wait_for([&] { return output->envelope_state(26,gain) && gain.level>held_level+480; });
        if (!output->sample_state(26,state) || state.frames!=held_position)
            throw std::runtime_error("Held source cursor advanced with its envelope");
        if (!output->configure_voice(26,{{0,0},{},1})) throw std::runtime_error("Second pitch resume failed");
        wait_for([&] { return output->sample_state(26,state) && state.frames>held_position+480; });
        const auto prior_cursor=state.frames;
        if (!output->revise_voices([&](std::span<const nt::audio::StreamState> states,std::vector<nt::audio::VoiceRevision>& edits) {
            if (states[26].frames<prior_cursor) return false;
            const auto begin=states[26].frames+128;
            edits.push_back({26,begin,std::vector<std::int16_t>(48000-begin,2048)});
            return true;
        }) || !output->sample_state(26,state) || state.frames<prior_cursor || !state.queued)
            throw std::runtime_error("Native PCM revision reset or lost the live playback cursor");
        if (output->revise_voices([](std::span<const nt::audio::StreamState>,std::vector<nt::audio::VoiceRevision>& edits) {
            edits.push_back({26,0,{123}}); return true;
        })) throw std::runtime_error("Native revision overwrote consumed PCM");
        wait_for([&] { return output->sample_state(26,state) && state.frames>prior_cursor+480; });
        if (!output->configure_voice(26,{{0,0},{},0}) || !output->release_voice(26))
            throw std::runtime_error("Held key-off failed");
        wait_for([&] { return output->envelope_state(26,gain) && !gain.active && !gain.level; });
        if (!output->stop_voice(26)) throw std::runtime_error("Held voice cleanup failed");
        output->close();
        if (output->ready() || output->voices()) throw std::runtime_error("Native audio resources survived close");
        if (!output->open() || !output->initialize(false)) throw std::runtime_error("Native audio output did not reopen");
        output->close();
#ifdef _WIN32
        if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) throw std::runtime_error("Test STA setup failed");
        const bool sta_audio=output->open() && output->initialize(false);
        output->close();
        APTTYPE apartment; APTTYPEQUALIFIER qualifier;
        const auto apartment_status=CoGetApartmentType(&apartment,&qualifier);
        CoUninitialize();
        if (!sta_audio || FAILED(apartment_status) || (apartment!=APTTYPE_STA && apartment!=APTTYPE_MAINSTA))
            throw std::runtime_error("Audio must coexist with and preserve the caller's STA apartment");
#endif
        std::cout<<"Native audio device, stereo buses, reverb impulse tails/reset, 48 voice controls, PCM consumption and cleanup verified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
