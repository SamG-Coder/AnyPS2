#pragma once
#include <nt/audio_envelope.hpp>
#include <xapobase.h>
#include <atomic>
#include <memory>

namespace nt::audio::windows {
struct EnvelopeObservation { std::atomic<std::uint32_t> value{0x10000}; };
struct EnvelopeParameters { EnvelopeSpec spec; bool release=false; };
class EnvelopeEffect final : public CXAPOParametersBase {
public:
    explicit EnvelopeEffect(std::shared_ptr<EnvelopeObservation> observation)
        : CXAPOParametersBase(&properties_,reinterpret_cast<BYTE*>(parameters_.data()),sizeof(EnvelopeParameters),FALSE),
          observation_(std::move(observation)) { envelope_.start(); }
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 inputs,const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* input,
                                            UINT32 outputs,const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* output) override {
        if (inputs!=1 || outputs!=1 || !input || !output || input->pFormat->nChannels!=1 ||
            output->pFormat->nChannels!=1 || input->pFormat->nSamplesPerSec!=48000 || output->pFormat->nSamplesPerSec!=48000)
            return XAPO_E_FORMAT_UNSUPPORTED;
        return CXAPOParametersBase::LockForProcess(inputs,input,outputs,output);
    }
    void STDMETHODCALLTYPE Process(UINT32,const XAPO_PROCESS_BUFFER_PARAMETERS* input,UINT32,
                                   XAPO_PROCESS_BUFFER_PARAMETERS* output,BOOL enabled) override {
        const auto parameters=*reinterpret_cast<const EnvelopeParameters*>(BeginProcess());
        if (parameters.release && !released_) { envelope_.release(); released_=true; }
        auto* destination=static_cast<float*>(output->pBuffer);
        const auto* source=static_cast<const float*>(input->pBuffer);
        const bool silent=input->BufferFlags==XAPO_BUFFER_SILENT;
        for (unsigned frame=0;frame<input->ValidFrameCount;++frame) {
            const auto gain=enabled ? envelope_.next(parameters.spec) : 1.0f;
            destination[frame]=silent ? 0.0f : source[frame]*gain;
        }
        output->ValidFrameCount=input->ValidFrameCount;
        output->BufferFlags=silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID;
        const auto state=envelope_.state();
        observation_->value.store(state.level|(state.active ? 0x10000U : 0U),std::memory_order_relaxed);
        EndProcess();
    }
private:
    inline static const XAPO_REGISTRATION_PROPERTIES properties_={
        {0x903f6f4a,0xf671,0x4b59,{0x8e,0x42,0xd7,0x39,0x0a,0x90,0x3a,0x81}},
        L"AnyPS2 native gain envelope",L"AnyPS2 contributors",1,0,XAPOBASE_DEFAULT_FLAG,1,1,1,1};
    std::array<EnvelopeParameters,3> parameters_{};
    std::shared_ptr<EnvelopeObservation> observation_;
    Envelope envelope_;
    bool released_=false;
};
}
