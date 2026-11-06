#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_clock.hpp>
#include <optional>
#include <algorithm>
#include <array>
#include <span>

namespace nt::ee {
// Native graphics lifecycle and nominal application display deadlines. These
// are independent of retired instructions and physical monitor presentation.
class NativeGraphics {
public:
    struct Profile {
        std::uint64_t status_reads=0,finish_not_ready=0,vsync_not_ready=0,buffer_changes=0;
        std::array<std::uint64_t,2> circuit_changes{};
        std::uint64_t vsync_clear_to_observe_ns=0,vsync_observations=0;
        std::uint64_t vsync_deadlines=0,vsync_coalesced=0,vsync_acknowledged=0,scanout_missed=0;
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
    Stop (*readback)(void*,std::span<std::uint8_t>)=nullptr;
    IoRead (*readback_status)(void*)=nullptr;
    bool readback_enabled() const { return readback_enabled_; }
    // Optional drawing owner. Rejection must preserve that owner's state.
    Stop (*reset_drawing)(void*)=nullptr;
    void* edge_context=nullptr;
    void (*display_edge)(void*,bool)=nullptr;
    // When supplied, this replaces display_edge. False means no acceptance;
    // retain the edge cursor and retry on a later service-pump iteration.
    bool (*try_display_edge)(void*,bool)=nullptr;
    void (*display_overflow)(void*)=nullptr;
    // Present the selected output at a display boundary, before execution can
    // observe VSYNC and begin overwriting a single-buffered frame.
    void* scanout_context=nullptr;
    void (*scanout_ready)(void*)=nullptr;
    Stop configure(std::uint32_t interlace,std::uint32_t standard,std::uint32_t frame) {
        if (!scan_supported(standard,interlace) || frame>1) return Stop::unsupported_system;
        if (!synchronize) return Stop::unsupported_system;
        const auto ready=synchronize(context);
        if (ready!=Stop::budget) return ready;
        const auto time=clock_();
        completed_lines_=horizontal_ticks(time);
        mode_=DisplayMode{interlace,standard,frame}; epoch_=time; ticks_=0; delivered_ticks_=0;
        flags_=0; field_=0; events_=0; edge_lost_=false; scanout_odd_=false;
        vsync_profile_start_.reset();
        return Stop::budget;
    }
    const std::optional<DisplayMode>& mode() const { return mode_; }
    // The SMODE2 INT and FFMD bits of the current mode, or zero when unconfigured.
    std::uint64_t scan_control() const { return mode_ ? mode_->interlace|(mode_->frame<<1) : 0; }
    // Display clocks per pixel of a digital mode, whose clocks are square. NTSC
    // and PAL return zero: their output keeps one window pixel per plan pixel.
    unsigned clocks_per_pixel() const {
        if (!mode_) return 0;
        return mode_->standard==0x50 ? 2 : (mode_->standard==0x51 || mode_->standard==0x52) ? 1 : 0;
    }
    bool half_height_output() const { return mode_ && mode_->interlace && mode_->frame; }
    bool scanout_odd() const { return scanout_odd_; }
    // Monotonic nominal line deadlines share the display clock, independent of
    // presentation rate. A mode reset freezes the count until reconfigured.
    std::uint64_t horizontal_ticks(std::uint64_t time) const {
        return completed_lines_+(mode_ ? half_line_ticks(time)/2 : 0);
    }
    static bool maps(std::uint32_t address) {
        return address==0x12000000 || address==0x12000020 ||
            (address>=0x12000070 && address<=0x120000a0 && address%16==0) ||
            address==0x120000e0 || address==0x12001000 || address==0x12001010 || address==0x12001040;
    }
    IoRead64 read(std::uint32_t address) {
        if (!maps(address)) return {Stop::unsupported_system};
        if (poll_drawing) { const auto result=poll_drawing(context); if (result!=Stop::budget) return {result}; }
        advance();
        if (address==0x12001040) return {Stop::budget,readback_enabled_ ? 1U : 0U};
        if (address==0x12000000) return {Stop::budget,output_.control};
        if (address==0x120000e0) return {Stop::budget,output_.background};
        if (address>=0x12000070 && address<=0x120000a0) {
            const auto slot=(address-0x12000070)/16;
            return {Stop::budget,slot&1 ? output_.displays[slot/2] : output_.buffers[slot/2]};
        }
        if (address==0x12000020) return {Stop::budget,scan_control()};
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
    std::uint64_t displayed_buffer(unsigned circuit) const { return output_.buffers[circuit]; }
    Stop write(std::uint32_t address,std::uint64_t value) {
        if (address==0x12001040) {
            if (value>1 || !synchronize) return Stop::unsupported_system;
            const auto ready=synchronize(context);
            if (ready!=Stop::budget) return ready;
            readback_enabled_=value!=0; return Stop::budget;
        }
        if (address==0x12000000) { output_.control=value&0xffffU; return Stop::budget; }
        if (address==0x120000e0) { output_.background=value&0xffffffU; return Stop::budget; }
        if (address>=0x12000070 && address<=0x120000a0 && address%16==0) {
            const auto slot=(address-0x12000070)/16;
            if (profile && !(slot&1) && output_.buffers[slot/2]!=value) { ++profile->buffer_changes; ++profile->circuit_changes[slot/2]; }
            (slot&1 ? output_.displays[slot/2] : output_.buffers[slot/2])=value;
            return Stop::budget;
        }
        if (address==0x12000020) {
            // INT and FFMD choose the scan type and field or frame output within
            // the configured standard, so display deadlines continue unchanged.
            // Power-saving DPMS states have no native output and remain refused.
            if (!mode_ || value>3 || !scan_supported(mode_->standard,static_cast<std::uint32_t>(value&1))) return Stop::unsupported_system;
            mode_->interlace=static_cast<std::uint32_t>(value&1);
            mode_->frame=static_cast<std::uint32_t>(value>>1);
            return Stop::budget;
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
                    completed_lines_=horizontal_ticks(clock_());
                    mask_=0x7f00; flags_=0; field_=0; events_=0; scanout_odd_=false; mode_.reset(); output_={}; readback_enabled_=false; ++resets_;
                    return Stop::budget;
                }
            }
            advance();
            if (profile && (value&8) && (flags_&8)) ++profile->vsync_acknowledged;
            flags_&=~static_cast<std::uint32_t>(value&31);
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
    // The scheduler must wake for VBLANK start as well as GS sync. Waiting a
    // whole service interval can cross both distinct edges before a callback
    // runs. A deadline already passed requests an immediate service poll.
    std::optional<std::uint64_t> event_wait_ns() const {
        if (!mode_) return {};
        const auto [period,blank,sync,numerator,denominator]=timing();
        const auto base=(ticks_/period)*period;
        const auto phase=ticks_%period;
        const auto target=base+(phase<blank ? blank : phase<sync ? sync : period);
        const auto deadline=epoch_+(target/numerator)*denominator+
            ((target%numerator)*denominator+numerator-1)/numerator;
        const auto now=clock_();
        return deadline>now ? deadline-now : 0;
    }
    // Time remaining to the real native display deadline. This does not set a
    // completion bit early, acknowledge it, or advance time by retired operations.
    // Missing configuration leaves the original compiled poll intact.
    std::optional<std::uint64_t> display_wait_ns() {
        advance();
        if (!mode_) return {};
        if (flags_&8) return 0;
        const auto [period,blank,sync,numerator,denominator]=timing();
        auto target=(ticks_/period)*period+sync;
        if (target<=ticks_) target+=period;
        const auto deadline=epoch_+(target/numerator)*denominator+
            ((target%numerator)*denominator+numerator-1)/numerator;
        const auto now=clock_();
        return deadline>now ? deadline-now : 0;
    }
private:
    // NTSC and PAL may be either; the digital modes each define one scan type.
    static bool scan_supported(std::uint32_t standard,std::uint32_t interlace) {
        return (standard==2 || standard==3) ? interlace<=1 :
            (standard==0x50 || standard==0x52) ? interlace==0 : standard==0x51 && interlace==1;
    }
    // In scan ticks per displayed field or frame. The duration of one tick is
    // denominator/numerator nanoseconds: half lines when interlaced or NTSC/PAL,
    // lines for digital progressive modes. Rates are 60000/1001 for NTSC and
    // 480p, and 50 or 60 otherwise.
    struct Timing { std::uint64_t period,blank,sync,numerator,denominator; };
    Timing timing() const {
        switch (mode_->standard) {
        case 3: return {625,576,582,1,32000};
        case 0x51: return {1125,1080,1084,27,400000};
        case 0x52: return {750,720,725,9,200000};
        default: return {525,480,487,63,2002000};
        }
    }
    static std::uint64_t edges(std::uint64_t ticks,std::uint32_t period,std::uint32_t offset) {
        return ticks<offset ? 0 : (ticks-offset)/period+1;
    }
    void event(std::uint32_t flag) {
        if (!(flags_&flag) && !(mask_&(flag<<8))) events_|=1;
        flags_|=flag;
    }
    std::uint64_t half_line_ticks(std::uint64_t time) const {
        if (time<epoch_) return ticks_;
        const auto elapsed=time-epoch_;
        // Reduced rational deadlines. Quotient/remainder avoids overflow and catch-up loops.
        const auto [period,blank,sync,numerator,denominator]=timing();
        return std::max(ticks_,(elapsed/denominator)*numerator+(elapsed%denominator)*numerator/denominator);
    }
    void advance() {
        if (!mode_) return;
        const auto ticks=half_line_ticks(clock_());
        if (ticks<ticks_) return;
        const auto [period64,blank64,sync64,numerator,denominator]=timing();
        const auto period=static_cast<std::uint32_t>(period64),blank=static_cast<std::uint32_t>(blank64),sync=static_cast<std::uint32_t>(sync64);
        if (display_edge || try_display_edge) {
            // Bound catch-up work and preserve chronological delivery. A
            // backpressured subscriber retains the last accepted deadline;
            // a legacy subscriber that cannot retry gets an explicit overflow.
            auto start=(delivered_ticks_/period)*period+blank;
            if (start<=delivered_ticks_) start+=period;
            auto end=(delivered_ticks_/period+1)*period;
            unsigned emitted=0;
            while (std::min(start,end)<=ticks && emitted<258) {
                const bool ending=end<start;
                const auto next=std::min(start,end);
                if (try_display_edge) {
                    if (!try_display_edge(edge_context,ending)) break;
                } else display_edge(edge_context,ending);
                delivered_ticks_=next;
                if (ending) end+=period; else start+=period;
                ++emitted;
            }
            if (std::min(start,end)<=ticks && !try_display_edge) {
                edge_lost_=true;
                if (display_overflow) display_overflow(edge_context);
                delivered_ticks_=ticks;
            }
        } else delivered_ticks_=ticks;
        if (ticks==ticks_) return;
        if (ticks/2>ticks_/2) event(4);
        const auto scanouts=edges(ticks,period,blank)-edges(ticks_,period,blank);
        const bool scanout=scanouts!=0;
        if (profile && scanouts>1) profile->scanout_missed+=scanouts-1;
        if (scanout) {
            events_|=1U<<2;
            // The completed field is still current at VBLANK start; FIELD
            // toggles at the later GS sync edge (zero denotes the even field).
            scanout_odd_=((edges(ticks,period,blank)-1)&1)!=0;
        }
        const auto syncs=edges(ticks,period,sync)-edges(ticks_,period,sync);
        if (syncs) {
            if (profile) {
                profile->vsync_deadlines+=syncs;
                profile->vsync_coalesced+=syncs-((flags_&8) ? 0U : 1U);
            }
            event(8);
        }
        field_=(edges(ticks,period,sync)&1) ? 0x2000 : 0;
        if (ticks/period>ticks_/period) events_|=1U<<3;
        ticks_=ticks;
        // Missed boundaries share the current image; do not present it repeatedly.
        if (scanout && scanout_ready) scanout_ready(scanout_context);
    }
    NativeClock clock_;
    std::optional<std::uint64_t> vsync_profile_start_;
    Output output_;
    bool edge_lost_=false,scanout_odd_=false,readback_enabled_=false;
    std::optional<DisplayMode> mode_;
    std::uint32_t mask_=0x7f00,flags_=0,field_=0,events_=0;
    std::uint64_t epoch_=0,ticks_=0,delivered_ticks_=0,resets_=0,completed_lines_=0;
};
}
