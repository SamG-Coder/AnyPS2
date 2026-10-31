#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_clock.hpp>
#include <optional>
#include <algorithm>

namespace nt::ee {
// Native graphics lifecycle and nominal application display deadlines. These
// are independent of retired instructions and physical monitor presentation.
class NativeGraphics {
public:
    explicit NativeGraphics(NativeClock clock=nullptr) : clock_(clock ? clock : native_now) {}
    struct DisplayMode { std::uint32_t interlace,standard,frame; };
    void* context=nullptr;
    Stop (*synchronize)(void*)=nullptr;
    // Optional drawing owner. Rejection must preserve that owner's state.
    Stop (*reset_drawing)(void*)=nullptr;
    void* edge_context=nullptr;
    void (*display_edge)(void*,bool)=nullptr;
    void (*display_overflow)(void*)=nullptr;
    Stop configure(std::uint32_t interlace,std::uint32_t standard,std::uint32_t frame) {
        if (interlace!=1 || (standard!=2 && standard!=3) || frame>1) return Stop::unsupported_system;
        if (!synchronize) return Stop::unsupported_system;
        const auto ready=synchronize(context);
        if (ready!=Stop::budget) return ready;
        mode_=DisplayMode{interlace,standard,frame}; epoch_=clock_(); ticks_=0;
        flags_=0; field_=0; events_=0; edge_lost_=false;
        return Stop::budget;
    }
    const std::optional<DisplayMode>& mode() const { return mode_; }
    static bool maps(std::uint32_t address) {
        return address==0x12000020 || address==0x12001000 || address==0x12001010;
    }
    IoRead64 read(std::uint32_t address) {
        if (!maps(address)) return {Stop::unsupported_system};
        advance();
        if (address==0x12000020) return {Stop::budget,mode_ ? mode_->interlace|(mode_->frame<<1) : 0};
        if (address==0x12001000) return {Stop::budget,0x4000U|field_|flags_};
        return {Stop::budget,mask_};
    }
    Stop write(std::uint32_t address,std::uint64_t value) {
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
                    mask_=0x7f00; flags_=0; field_=0; events_=0; mode_.reset(); ++resets_;
                    return Stop::budget;
                }
            }
            advance(); flags_&=~static_cast<std::uint32_t>(value&31);
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
        if (edges(ticks,period,sync)>edges(ticks_,period,sync)) event(8);
        field_=(edges(ticks,period,sync)&1) ? 0x2000 : 0;
        if (ticks/period>ticks_/period) events_|=1U<<3;
        ticks_=ticks;
    }
    NativeClock clock_;
    bool edge_lost_=false;
    std::optional<DisplayMode> mode_;
    std::uint32_t mask_=0x7f00,flags_=0,field_=0,events_=0;
    std::uint64_t epoch_=0,ticks_=0,resets_=0;
};
}
