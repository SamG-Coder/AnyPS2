#include <nt/audio.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <array>

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
        std::array<IXAudio2SourceVoice*,48> pending{};
        const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,48000,96000,2,16,0};
        for (auto& voice:pending) {
            auto result=engine_->CreateSourceVoice(&voice,&format,0,4.0f);
            if (SUCCEEDED(result)) result=voice->SetVolume(0.0f);
            if (SUCCEEDED(result)) result=voice->SetFrequencyRatio(16383.0f/4096.0f);
            if (FAILED(result)) {
                for (auto created:pending) if (created) created->DestroyVoice();
                error_=static_cast<std::uint32_t>(result); return false;
            }
        }
        for (auto voice:voices_) if (voice) voice->DestroyVoice();
        voices_=pending; error_=0;
        return true;
    }
    void close() override {
        for (auto& voice:voices_) if (voice) { voice->DestroyVoice(); voice=nullptr; }
        if (master_) { master_->DestroyVoice(); master_=nullptr; }
        if (engine_) { engine_->Release(); engine_=nullptr; }
        if (com_) { CoUninitialize(); com_=false; }
    }
    bool ready() const override { return master_!=nullptr; }
    unsigned voices() const override { return voices_.front() ? 48U : 0U; }
    std::uint32_t error() const override { return error_; }
private:
    IXAudio2* engine_=nullptr;
    IXAudio2MasteringVoice* master_=nullptr;
    std::array<IXAudio2SourceVoice*,48> voices_{};
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
    void close() override {}
    bool ready() const override { return false; }
    unsigned voices() const override { return 0; }
    std::uint32_t error() const override { return 1; }
};
}
std::unique_ptr<Output> make_output() { return std::make_unique<UnavailableOutput>(); }
}
#endif
