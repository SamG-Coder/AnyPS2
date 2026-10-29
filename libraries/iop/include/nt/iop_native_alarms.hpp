#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iop_native_code.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>

namespace nt::iop {
class NativeAlarmServices final : public NativeServices, public NativeCallbackSource {
public:
    using Clock=std::function<std::uint64_t()>;
    explicit NativeAlarmServices(Clock clock={},std::uint32_t frequency=36864000)
        : frequency_(frequency),clock_(std::move(clock)) {
        if (frequency<10000) throw std::invalid_argument("Native alarm clock frequency is too low");
        if (!clock_) clock_=[epoch=std::chrono::steady_clock::now(),frequency] {
            const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch).count());
            return (ns/1000000000)*frequency+(ns%1000000000)*frequency/1000000000;
        };
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thbase || request.selector<34 || request.selector>38) return {};
        const auto op=request.selector;
        const auto& a=request.arguments;
        const auto returned=[](std::int32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (op!=34 && request.interrupt_context!=(op==36 || op==38)) return returned(-100);
        if (op==34) {
            const auto checked=output(ram,a[0],8);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            const auto offset=*ram_offset(a[0],4,ram.size());
            // Sample once so high and low words always describe the same instant.
            const auto now=clock_();
            store_native_word(ram,offset,static_cast<std::uint32_t>(now));
            store_native_word(ram,offset+4,static_cast<std::uint32_t>(now>>32)); return returned();
        }
        if (op==37 || op==38) {
            for (auto it=alarms_.begin();it!=alarms_.end();++it)
                if (it->second.address==a[0] && it->second.argument==a[1]) { alarms_.erase(it); return returned(); }
            return returned(-105);
        }
        for (const auto& [id,alarm] : alarms_)
            if (alarm.address==a[1] && alarm.argument==a[2]) return returned(-104);
        if (alarms_.size()>=1024) return returned(-400);
        const auto offset=ram_offset(a[0],4,ram.size());
        if (!offset || ram.size()-*offset<8 || 0x200000-*offset<8) return {NativeServiceStatus::address_fault,0,a[0]};
        const auto entry=ram_offset(a[1],4,0x200000);
        if (!entry || !nt_iop_native_instruction(*entry)) return {NativeServiceStatus::unmapped_code,0,a[1]};
        const auto requested=std::uint64_t(load_native_word(ram,*offset))|(std::uint64_t(load_native_word(ram,*offset+4))<<32);
        const auto interval=std::max<std::uint64_t>(requested,minimum());
        if (interval!=requested) {
            const auto checked=output(ram,a[0],4);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        const auto now=clock_();
        if (interval>std::numeric_limits<std::uint64_t>::max()-now || next_id_==std::numeric_limits<std::uint64_t>::max()) return {};
        alarms_.emplace(next_id_++,Alarm{now+interval,a[1],a[2]});
        if (interval!=requested) store_native_word(ram,*offset,static_cast<std::uint32_t>(interval));
        return returned();
    }
    std::optional<NativeCallback> take() override {
        if (active_) return {};
        auto selected=alarms_.end();
        for (auto it=alarms_.begin();it!=alarms_.end();++it)
            if (selected==alarms_.end() || it->second.deadline<selected->second.deadline) selected=it;
        if (selected==alarms_.end() || selected->second.deadline>clock_()) return {};
        active_=*selected; alarms_.erase(selected);
        return NativeCallback{active_->second.address,active_->second.argument};
    }
    void finish(std::uint32_t interval) override {
        if (!active_) throw std::logic_error("Native alarm completion has no active callback");
        if (interval) {
            const auto delay=std::max(interval,minimum());
            if (delay>std::numeric_limits<std::uint64_t>::max()-active_->second.deadline)
                throw std::overflow_error("Native alarm deadline overflow");
            active_->second.deadline+=delay; alarms_.insert(*active_);
        }
        active_.reset();
    }
    std::optional<std::chrono::nanoseconds> delay() const {
        if (alarms_.empty()) return {};
        auto deadline=std::numeric_limits<std::uint64_t>::max();
        for (const auto& [id,alarm] : alarms_) deadline=std::min(deadline,alarm.deadline);
        const auto now=clock_();
        const auto ticks=deadline>now ? std::min<std::uint64_t>(deadline-now,frequency_) : 0;
        return std::chrono::nanoseconds((ticks*1000000000+frequency_-1)/frequency_);
    }
private:
    static NativeServiceReply output(std::span<std::uint8_t> ram,std::uint32_t pointer,std::uint32_t size) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,pointer};
        return {NativeServiceStatus::returned};
    }
    std::uint32_t minimum() const { return frequency_/10000; }
    struct Alarm { std::uint64_t deadline; std::uint32_t address,argument; };
    std::uint32_t frequency_;
    Clock clock_;
    std::map<std::uint64_t,Alarm> alarms_;
    std::optional<std::pair<std::uint64_t,Alarm>> active_;
    std::uint64_t next_id_=1;
};
}
