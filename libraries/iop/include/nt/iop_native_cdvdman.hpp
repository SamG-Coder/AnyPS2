#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <chrono>
#include <functional>

namespace nt::iop {
// Ordinal 24 writes the eight-byte sceCdCLOCK from the host clock in Japan
// standard time. Every other cdvdman ordinal stays unsupported.
class NativeCdvdmanServices final : public NativeServices {
public:
    using Clock=std::function<std::chrono::sys_seconds()>;
    explicit NativeCdvdmanServices(Clock clock={}) : clock_(std::move(clock)) {
        if (!clock_) clock_=[] {
            return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        };
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::cdvdman || request.selector!=24) return {};
        const auto pointer=request.arguments[0];
        const auto checked=output(ram,pointer,8);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        using namespace std::chrono;
        const auto now=clock_()+hours{9};
        const auto day=floor<days>(now);
        const year_month_day date{day};
        const hh_mm_ss time{now-day};
        const auto year_value=int(date.year());
        if (year_value<2000 || year_value>2099) return {};
        const auto bcd=[](unsigned value) { return static_cast<std::uint8_t>(((value/10)<<4)|(value%10)); };
        const auto offset=*ram_offset(pointer,4,ram.size());
        const std::uint8_t bytes[8]{
            0,
            bcd(static_cast<unsigned>(time.seconds().count())),
            bcd(static_cast<unsigned>(time.minutes().count())),
            bcd(static_cast<unsigned>(time.hours().count())),
            0,
            bcd(static_cast<unsigned>(date.day())),
            static_cast<std::uint8_t>(bcd(static_cast<unsigned>(date.month()))&0x7f),
            bcd(static_cast<unsigned>(year_value-2000))};
        for (unsigned index=0;index<8;++index) ram[offset+index]=bytes[index];
        return {NativeServiceStatus::returned,1};
    }
private:
    static NativeServiceReply output(std::span<std::uint8_t> ram,std::uint32_t pointer,std::uint32_t size) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,pointer};
        return {NativeServiceStatus::returned};
    }
    Clock clock_;
};
}
