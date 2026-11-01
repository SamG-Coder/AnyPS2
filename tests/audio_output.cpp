#include <nt/audio.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        auto output=nt::audio::make_output();
        if (output->ready() || output->voices()) throw std::runtime_error("Output started without initialization");
        if (!output->open() || !output->ready()) throw std::runtime_error("Cannot open the native output device");
        for (bool preserve : {false,true})
            if (!output->initialize(preserve) || output->voices()!=48 || output->error())
                throw std::runtime_error("Native source-voice initialization failed");
        nt::audio::CoreMix core;
        core.output={0.5f,-1.0f}; core.external_direct={0.25f,0.75f};
        for (unsigned c=0;c<2;++c)
            if (!output->configure_core(c,core)) throw std::runtime_error("Native stereo bus configuration failed");
        for (unsigned voice=0;voice<48;++voice) {
            if (!output->configure_voice(voice,{{0.5f,-0.5f},{0,0},0.5f}) ||
                !output->configure_voice(voice,{{0,0},{0,0},0}))
                throw std::runtime_error("Native voice gain, routing or zero-rate suspension failed");
        }
        if (output->configure_voice(48,{}) || output->configure_core(2,{}))
            throw std::runtime_error("Invalid native sound channel accepted");
        output->close();
        if (output->ready() || output->voices()) throw std::runtime_error("Native audio resources survived close");
        if (!output->open() || !output->initialize(false)) throw std::runtime_error("Native audio output did not reopen");
        output->close();
        std::cout<<"Native audio device, stereo buses, 48 voice gain/rate updates, reinitialization and cleanup verified. No samples submitted.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
