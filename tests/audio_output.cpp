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
        output->close();
        if (output->ready() || output->voices()) throw std::runtime_error("Native audio resources survived close");
        if (!output->open() || !output->initialize(false)) throw std::runtime_error("Native audio output did not reopen");
        output->close();
        std::cout<<"Native audio device, 48 stopped PCM voices, reinitialization and cleanup verified. No samples submitted.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
