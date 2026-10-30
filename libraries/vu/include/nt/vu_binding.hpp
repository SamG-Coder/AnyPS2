#pragma once
#include <nt/vu_dispatch.hpp>

namespace nt::vu {
struct UploadView {
    unsigned destination;
    std::span<const std::uint8_t> bytes;
};

// Binds one conversion artifact to matching uploads. Original bytes are used
// solely for exact identity checks; execution calls the compiled Program only.
class Binding {
public:
    Binding(Program program,unsigned destination,std::span<const std::uint8_t> image)
        : program_(std::move(program)),destination_(destination),image_(image.begin(),image.end()),
          present_(image.size()/8,false) {
        if (image.empty() || image.size()%8 ||
            std::uint64_t(destination)+image.size()/8>(program_.unit() ? 2048U : 512U) ||
            !program_.within(destination,std::uint64_t(destination)+image.size()/8))
            throw std::invalid_argument("Invalid compiled VU image extent");
    }

    // Validate every chunk before changing coverage. Rejecting an upload batch
    // leaves the binding unchanged, just as the caller must leave its packet state.
    bool upload(unsigned unit,std::span<const UploadView> uploads) {
        if (unit!=program_.unit() || uploads.empty()) return false;
        for (const auto& upload:uploads) {
            if (upload.bytes.empty() || upload.bytes.size()%8 || upload.destination<destination_) return false;
            const auto offset=std::uint64_t(upload.destination-destination_)*8;
            if (offset>image_.size() || upload.bytes.size()>image_.size()-offset ||
                !std::equal(upload.bytes.begin(),upload.bytes.end(),image_.begin()+offset)) return false;
        }
        for (const auto& upload:uploads) {
            const auto begin=upload.destination-destination_;
            std::fill(present_.begin()+begin,present_.begin()+begin+upload.bytes.size()/8,true);
        }
        return true;
    }
    bool ready() const { return std::all_of(present_.begin(),present_.end(),[](bool value) { return value; }); }
    unsigned unit() const { return program_.unit(); }
    void reset() { std::fill(present_.begin(),present_.end(),false); }
    RunResult launch(State& state,unsigned entry,std::uint64_t budget) const {
        if (!ready()) return {RunStatus::unbound_program,0};
        if (state.graphics_pending) return {RunStatus::graphics_pending,0};
        // Zero budget must not alter even the caller's continuation.
        if (!budget) return {RunStatus::budget_exhausted,0};
        state.graphics_completes=0;
        state.next_pair=entry;
        return program_.run(state,budget);
    }
    RunResult resume(State& state,std::uint64_t budget) const {
        return ready() ? program_.run(state,budget) : RunResult{RunStatus::unbound_program,0};
    }
private:
    Program program_;
    unsigned destination_;
    std::vector<std::uint8_t> image_;
    std::vector<bool> present_;
};
}
