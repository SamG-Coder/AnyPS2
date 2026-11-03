#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_clock.hpp>
#include <optional>
#include <algorithm>
#include <array>

namespace nt::ee {
// Native graphics lifecycle and nominal application display deadlines. These
// are independent of retired instructions and physical monitor presentation.
class NativeGraphics {
public:
    struct Profile {
        std::uint64_t status_reads=0,finish_not_ready=0,vsync_not_ready=0,buffer_changes=0;
        std::uint64_t vsync_clear_to_observe_ns=0,vsync_observations=0;
    };
    Profile* profile=nullptr;
    explicit NativeGraphics(NativeClock clock=nullptr) : clock_(clock ? clock : native_now) {}
    struct DisplayMode { std::uint32_t interlace,standard,frame; };
    struct Output { std::uint64_t control=0,background=0; std::array<std::uint64_t,2> buffers{},displays{}; };
    const Output& output() const { return output_; }
    // Called only after the native renderer has completed a submitted FINISH
    // fence. The existing interrupt owner delivers the resulting event.
    void drawing_completed() { event(2); }
    void* context=nullptr;
    Stop (*synchronize)(void*)=nullptr;
    Stop (*poll_drawing)(void*)=nullptr;
    // Optional drawing owner. Rejection must preserve that owner's state.
    Stop (*reset_drawing)(void*)=nullptr;
    void* edge_context=nullptr;
    void (*display_edge)(void*,bool)=nullptr;
    void (*display_overflow)(void*)=nullptr;
    // Present the selected output at a display boundary, before execution can
    // observe VSYNC and begin overwriting a single-buffered frame.
    void* scanout_context=nullptr;
    void (*scanout_ready)(void*)=nullptr;
    Stop configure(std::uint32_t interlace,std::uint32_t standard,std::uint32_t frame) {
        if (interlace!=1 || (standard!=2 && standard!=3) || frame>1) return Stop::unsupported_system;
        if (!synchronize) return Stop::unsupported_system;
        const auto ready=synchronize(context);
        if (ready!=Stop::budget) return ready;
        mode_=DisplayMode{interlace,standard,frame}; epoch_=clock_(); ticks_=0;
        flags_=0; field_=0; events_=0; edge_lost_=false;
        vsync_profile_start_.reset();
        return Stop::budget;
    }
    const std::optional<DisplayMode>& mode() const { return mode_; }
    bool half_height_output() const { return mode_ && mode_->interlace && mode_->frame; }
    static bool maps(std::uint32_t address) {
        return address==0x12000000 || address==0x12000020 ||
            (address>=0x12000070 && address<=0x120000a0 && address%16==0) ||
            address==0x120000e0 || address==0x12001000 || address==0x12001010;
    }
    IoRead64 read(std::uint32_t address) {
        if (!maps(address)) return {Stop::unsupported_system};
        if (poll_drawing) { const auto result=poll_drawing(context); if (result!=Stop::budget) return {result}; }
        advance();
        if (address==0x12000000) return {Stop::budget,output_.control};
        if (address==0x120000e0) return {Stop::budget,output_.background};
        if (address>=0x12000070 && address<=0x120000a0) {
            const auto slot=(address-0x12000070)/16;
            return {Stop::budget,slot&1 ? output_.displays[slot/2] : output_.buffers[slot/2]};
        }
        if (address==0x12000020) return {Stop::budget,mode_ ? mode_->interlace|(mode_->frame<<1) : 0};
        if (address==0x12001000) {
            if (profile) { ++profile->status_reads; profile->finish_not_ready+=!(flags_&2); profile->vsync_not_ready+=!(flags_&8); }
            if (profile && vsync_profile_start_ && (flags_&8)) {
                const auto observed=clock_();
                if (observed>=*vsync_profile_start_) profile->vsync_clear_to_observe_ns+=observed-*vsync_profile_start_;
                ++profile->vsync_observations; vsync_profile_start_.reset();
            }
            return {Stop::budget,0x4000U|field_|flags_};
        }
        return {Stop::budget,mask_};
    }
    Stop write(std::uint32_t address,std::uint64_t value) {
        if (address==0x12000000) { output_.control=value&0xffffU; return Stop::budget; }
        if (address==0x120000e0) { output_.background=value&0xffffffU; return Stop::budget; }
        if (address>=0x12000070 && address<=0x120000a0 && address%16==0) {
            const auto slot=(address-0x12000070)/16;
            if (profile && !(slot&1) && output_.buffers[slot/2]!=value) ++profile->buffer_changes;
            (slot&1 ? output_.displays[slot/2] : output_.buffers[slot/2])=value;
            return Stop::budget;
        }
        if (address==0x12000020) {
            return mode_ && value==(mode_->interlace|(mode_->frame<<1)) ? Stop::budget : Stop::unsupported_system;
        }
        if (address==0x12001000) {
            if (value&0x300) {
                if (!synchronize) return Stop::unsupported_system;
                const auto result=synchronize(context);
                if (result!=Stop::budget) return result;
                if (value&0x200) {
                    if (reset_drawing) {
                        const auto reset=reset_drawing(context);
                        if (reset!=Stop::budget) return reset;
                    }
                    mask_=0x7f00; flags_=0; field_=0; events_=0; mode_.reset(); output_={}; ++resets_;
                    return Stop::budget;
                }
            }
            advance(); flags_&=~static_cast<std::uint32_t>(value&31);
            if (profile && (value&8)) vsync_profile_start_=clock_();
            return Stop::budget;
        }
        if (address==0x12001010) {
            advance(); const auto next=static_cast<std::uint32_t>(value&0x1f00)|0x6000;
            if (flags_&(mask_>>8)&~(next>>8)) events_|=1;
            mask_=next; return Stop::budget;
        }
        return Stop::unsupported_system;
    }
    std::uint32_t poll() { advance(); const auto result=events_; events_=0; return result; }
    std::uint64_t resets() const { return resets_; }
    bool edge_overflow() const { return edge_lost_; }
private:
    static std::uint64_t edges(std::uint64_t ticks,std::uint32_t period,std::uint32_t offset) {
        return ticks<offset ? 0 : (ticks-offset)/period+1;
    }
    void event(std::uint32_t flag) {
        if (!(flags_&flag) && !(mask_&(flag<<8))) events_|=1;
        flags_|=flag;
    }
    void advance() {
        if (!mode_) return;
        const auto time=clock_();
        if (time<epoch_) return;
        const auto elapsed=time-epoch_;
        // Reduced rational half-line deadlines: NTSC 60000/1001 fields/sec,
        // PAL 50 fields/sec. Quotient/remainder avoids overflow and catch-up loops.
        const std::uint64_t numerator=mode_->standard==2 ? 63 : 1;
        const std::uint64_t denominator=mode_->standard==2 ? 2002000 : 32000;
        const auto ticks=(elapsed/denominator)*numerator+(elapsed%denominator)*numerator/denominator;
        if (ticks<=ticks_) return;
        const auto period=mode_->standard==2 ? 525U : 625U;
        const auto blank=mode_->standard==2 ? 480U : 576U;
        const auto sync=mode_->standard==2 ? 487U : 582U;
        if (display_edge) {
            // Deliver chronological edges to native IOP subscribers. Bound
            // catch-up work; the recipient reports overflow rather than losing
            // callback completions silently.
            auto start=(ticks_/period)*period+blank;
            if (start<=ticks_) start+=period;
            auto end=(ticks_/period+1)*period;
            unsigned emitted=0;
            while (std::min(start,end)<=ticks && emitted<258) {
                const bool ending=end<start;
                display_edge(edge_context,ending);
                if (ending) end+=period; else start+=period;
                ++emitted;
            }
            if (std::min(start,end)<=ticks) {
                edge_lost_=true;
                if (display_overflow) display_overflow(edge_context);
            }
        }
        if (ticks/2>ticks_/2) event(4);
        if (edges(ticks,period,blank)>edges(ticks_,period,blank)) events_|=1U<<2;
        const bool scanout=edges(ticks,period,sync)>edges(ticks_,period,sync);
        if (scanout) event(8);
        field_=(edges(ticks,period,sync)&1) ? 0x2000 : 0;
        if (ticks/period>ticks_/period) events_|=1U<<3;
        ticks_=ticks;
        // Missed boundaries share the current image; do not present it repeatedly.
        if (scanout && scanout_ready) scanout_ready(scanout_context);
    }
    NativeClock clock_;
    std::optional<std::uint64_t> vsync_profile_start_;
    Output output_;
    bool edge_lost_=false;
    std::optional<DisplayMode> mode_;
    std::uint32_t mask_=0x7f00,flags_=0,field_=0,events_=0;
    std::uint64_t epoch_=0,ticks_=0,resets_=0;
};
}
