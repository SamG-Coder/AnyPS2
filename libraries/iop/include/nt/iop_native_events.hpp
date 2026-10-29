#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_waits.hpp>
#include <map>
#include <vector>

namespace nt::iop {
// Native event objects own their state; guest handles never expose host pointers.
// Blocking waits require an explicitly connected continuation scheduler.
class NativeEventServices final : public NativeServices {
public:
    explicit NativeEventServices(NativeThreadWaits* waits=nullptr) : waits_(waits) {}
    std::int32_t create(std::uint32_t attributes,std::uint32_t option,std::uint32_t initial) {
        if (attributes&~2U) return -401;
        if (events_.size()>=4096 || next_id_>0x7fffffffU) return -400;
        events_.emplace(next_id_,Event{attributes,option,initial,initial,{}});
        return static_cast<std::int32_t>(next_id_++);
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thevent) return {};
        const auto op=request.selector;
        if (!((op>=4 && op<=11) || op==13 || op==14)) return {};
        const auto returned=[](std::int32_t value=0) {
            return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)};
        };
        const bool interrupt_call=op==7 || op==9 || op==14;
        if (request.interrupt_context!=interrupt_call) return returned(-100);
        const auto& a=request.arguments;
        if (op==4) {
            const auto source=ram_offset(a[0],4,ram.size());
            if (!source || ram.size()-*source<12 || 0x200000-*source<12)
                return {NativeServiceStatus::address_fault,0,a[0]};
            return returned(create(load_native_word(ram,*source),load_native_word(ram,*source+4),load_native_word(ram,*source+8)));
        }
        if (op==10 || op==11) {
            if (a[2]&~0x11U) return returned(-405);
            if (!a[1]) return returned(-423);
        }
        const auto found=events_.find(a[0]);
        if (found==events_.end()) return returned(-409);
        auto& event=found->second;
        if (op==5) {
            for (const auto& waiter : event.waiters) waits_->resume_wait(waiter.thread,-425);
            events_.erase(found); return returned();
        }
        if (op==6 || op==7) {
            if (!a[1]) return returned();
            // Validate every selected output before committing bits, writes or wakeups.
            auto pattern=event.current|a[1];
            for (const auto& waiter : event.waiters) {
                if (!matches(pattern,waiter.bits,waiter.mode)) continue;
                if (waiter.output) {
                    const auto checked=check_output(ram,waiter.output,4);
                    if (checked.status!=NativeServiceStatus::returned) return checked;
                }
                if (waiter.mode&0x10) pattern=0;
            }
            event.current|=a[1];
            for (auto waiter=event.waiters.begin();waiter!=event.waiters.end();) {
                if (!matches(event.current,waiter->bits,waiter->mode)) { ++waiter; continue; }
                if (waiter->output) store_native_word(ram,*ram_offset(waiter->output,4,ram.size()),event.current);
                if (waiter->mode&0x10) event.current=0;
                waits_->resume_wait(waiter->thread,0);
                waiter=event.waiters.erase(waiter);
            }
            return returned();
        }
        // ClearEventFlag accepts a keep mask, not a mask of bits to remove.
        if (op==8 || op==9) { event.current&=a[1]; return returned(); }
        if (op==10 || op==11) {
            if (!(event.attributes&2) && !event.waiters.empty()) return returned(-422);
            if (!matches(event.current,a[1],a[2])) {
                if (op==11) return returned(-421);
                const auto thread=waits_ ? waits_->current_waiter() : std::nullopt;
                if (!thread) return {};
                if (a[3]) {
                    const auto checked=check_output(ram,a[3],4);
                    if (checked.status!=NativeServiceStatus::returned) return checked;
                }
                event.waiters.push_back({*thread,a[1],a[2],a[3]});
                waits_->suspend_wait(*thread,a[0],NativeWaitKind::event);
                return {NativeServiceStatus::yielded};
            }
            if (a[3]) {
                const auto checked=check_output(ram,a[3],4);
                if (checked.status!=NativeServiceStatus::returned) return checked;
                store_native_word(ram,*ram_offset(a[3],4,ram.size()),event.current);
            }
            if (a[2]&0x10) event.current=0;
            return returned();
        }
        const auto checked=check_output(ram,a[1],20);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto destination=*ram_offset(a[1],4,ram.size());
        const std::array values{event.attributes,event.option,event.initial,event.current,static_cast<std::uint32_t>(event.waiters.size())};
        for (unsigned index=0;index<values.size();++index) store_native_word(ram,destination+index*4,values[index]);
        return returned();
    }
private:
    static NativeServiceReply check_output(std::span<std::uint8_t> ram,std::uint32_t address,std::uint32_t size) {
        const auto destination=ram_offset(address,4,ram.size());
        if (!destination || size>ram.size()-*destination || size>0x200000-*destination)
            return {NativeServiceStatus::address_fault,0,address};
        for (auto at=*destination;at<*destination+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    static bool matches(std::uint32_t pattern,std::uint32_t bits,std::uint32_t mode) {
        return (mode&1) ? (pattern&bits)!=0 : (pattern&bits)==bits;
    }
    struct Waiter { std::uint32_t thread,bits,mode,output; };
    struct Event { std::uint32_t attributes,option,initial,current; std::vector<Waiter> waiters; };
    NativeThreadWaits* waits_;
    std::map<std::uint32_t,Event> events_;
    std::uint32_t next_id_=1;
};
}
