#pragma once
#include <nt/native_scanout.hpp>
#include <nt/pad_script.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <vector>

namespace probe {
// Diagnostic: for each scripted press, the number of presented display updates
// and milliseconds until the shown image visibly changes. It includes the
// program's own input handling and every native stage up to scanout (not the
// host swapchain or monitor). Frames are read back only around presses.
// Pixels that change anywhere in the lead window before the press are
// animation and are ignored, so moving scenes still measure the response.
class InputLatency {
public:
    explicit InputLatency(const nt::input::PadScript& script) {
        for (const auto& press:script.presses()) starts_.push_back(press.start_ms);
        std::sort(starts_.begin(),starts_.end());
    }
    bool wants(std::uint64_t now_ms) const {
        return next_<starts_.size() && now_ms+lead_ms>=starts_[next_];
    }
    void observe(std::uint64_t now_ms,const nt::graphics::Scanout& frame) {
        if (next_>=starts_.size()) return;
        const auto start=starts_[next_];
        const auto limit=next_+1<starts_.size() ? std::min(starts_[next_+1],start+timeout_ms) : start+timeout_ms;
        if (now_ms<start) {
            if (!baseline_.pixels.empty() && baseline_.width==frame.width && baseline_.height==frame.height &&
                still_.size()==frame.pixels.size()) {
                for (std::size_t i=0;i<frame.pixels.size();++i) still_[i]=still_[i] && baseline_.pixels[i]==frame.pixels[i];
            } else still_.assign(frame.pixels.size(),true);
            baseline_=frame; frames_=0; return;
        }
        ++frames_;
        if (!baseline_.pixels.empty() && changed(baseline_,frame,still_)) {
            results_.push_back({start,frames_,now_ms-start,true});
            advance();
        } else if (now_ms>=limit || baseline_.pixels.empty()) {
            results_.push_back({start,frames_,now_ms-start,false});
            advance();
        }
    }
    void report(std::ostream& out) const {
        for (const auto& r:results_)
            out<<"input_latency press_ms="<<r.start<<" updates="<<r.frames<<" ms="<<r.ms<<" changed="<<r.changed<<'\n';
    }
private:
    static constexpr std::uint64_t lead_ms=300,timeout_ms=2000;
    // A visible response: enough pixels change by a clear amount. Sub-pixel
    // jitter and small animations stay below this bound.
    static bool changed(const nt::graphics::Scanout& a,const nt::graphics::Scanout& b,const std::vector<bool>& still) {
        if (a.width!=b.width || a.height!=b.height) return true;
        std::size_t count=0;
        for (std::size_t i=0;i<a.pixels.size();++i) {
            if (i<still.size() && !still[i]) continue;
            int largest=0;
            for (unsigned shift=0;shift<24;shift+=8)
                largest=std::max(largest,std::abs(int((a.pixels[i]>>shift)&255)-int((b.pixels[i]>>shift)&255)));
            count+=largest>48;
        }
        return count*400>a.pixels.size();
    }
    void advance() { ++next_; baseline_={}; still_.clear(); frames_=0; }
    struct Result { std::uint64_t start; unsigned frames; std::uint64_t ms; bool changed; };
    std::vector<std::uint64_t> starts_;
    std::vector<Result> results_;
    nt::graphics::Scanout baseline_;
    std::vector<bool> still_;
    std::size_t next_=0;
    unsigned frames_=0;
};
}
