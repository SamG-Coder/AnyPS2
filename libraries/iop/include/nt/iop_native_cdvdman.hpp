#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iso9660.hpp>
#include <algorithm>
#include <chrono>
#include <functional>

namespace nt::iop {
// Native ISO reads share the initialized EE disc session. File I/O completes
// before buffers are published; its compiled callback runs on the existing
// interrupt owner. No optical timing or DMA register model is required.
class NativeCdvdmanServices final : public NativeServices,public NativeCallbackSource {
public:
    using Clock=std::function<std::chrono::sys_seconds()>;
    using Volume=std::function<disc::Iso9660*()>;
    void attach(Volume volume,std::function<std::uint32_t()> media) {
        volume_=std::move(volume); media_=std::move(media);
    }
    // The application owner must opt in and consume exit_requested(). This
    // replaces console power control with process lifecycle, never host power.
    void enable_application_exit() { application_exit_enabled_=true; }
    bool exit_requested() const { return exit_requested_; }
    bool request_close() {
        if (!application_exit_enabled_) return false;
        if (close_requested_ || exit_requested_) return true;
        close_requested_=true;
        if (power_callback_) power_pending_=power_callback_;
        else exit_requested_=true;
        return true;
    }
    explicit NativeCdvdmanServices(Clock clock={}) : clock_(std::move(clock)) {
        if (!clock_) clock_=[] {
            return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        };
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::cdvdman) return {};
        // Console-specific iLink identity has no native PC provider. Binding
        // this known ABI lets unrelated compiled paths remain executable.
        if (request.selector==22) return {NativeServiceStatus::unavailable};
        if (request.selector==74 || request.selector==78) return power_call(request,ram);
        // sceCdRC differs from sceCdReadClock only by keeping the month high
        // bit, which this host clock never sets.
        if (request.selector!=24 && request.selector!=51) return disc_call(request,ram);
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
    std::optional<NativeCallback> take() override {
        if (!pending_ && power_pending_) { pending_=power_pending_; power_pending_.reset(); }
        if (active_ || !pending_) return {};
        auto result=pending_; pending_.reset(); active_=true; return result;
    }
    void finish(std::uint32_t) override { active_=false; }
private:
    NativeServiceReply power_call(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!application_exit_enabled_) return {NativeServiceStatus::unavailable};
        const auto pointer=request.arguments[0];
        if (request.selector==78) {
            if (pointer) {
                const auto target=ram_offset(pointer,4,ram.size());
                if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,pointer};
            }
            const auto previous=power_callback_ ? power_callback_->address : 0U;
            power_callback_.reset();
            if (pointer) power_callback_=NativeCallback{pointer,request.arguments[1],{},request.global_pointer};
            return {NativeServiceStatus::returned,previous};
        }
        if (!pointer) return {NativeServiceStatus::address_fault,0,pointer};
        const auto checked=output(ram,pointer,4);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        store_native_word(ram,*ram_offset(pointer,4,ram.size()),0);
        exit_requested_=true;
        return {NativeServiceStatus::returned,1};
    }
    NativeServiceReply disc_call(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!volume_ || !media_) return {};
        const auto returned=[](std::uint32_t value) { return NativeServiceReply{NativeServiceStatus::returned,value}; };
        auto* volume=volume_();
        const auto& args=request.arguments;
        switch (request.selector) {
        case 5:
            if (pending_) return returned(0);
            if (!volume) { error_=0x12; return returned(0); }
            // No spindle exists; reads already resume a stopped volume. Retain the
            // public state and completion edge.
            stopped_=false; error_=0;
            if (callback_) pending_=NativeCallback{callback_,5,{}};
            return returned(1);
        case 6: return read(request,ram,volume);
        case 10: return search(request,ram,volume);
        case 8: return returned(error_);
        case 11:
            // Both IOP reads and the native EE read RPC publish their bytes
            // synchronously. Modes 16/17 also include RPC/stream work: no
            // streaming request is accepted by these providers yet. Callback
            // delivery is independent and must not be consumed by this query.
            // Timed waits (3/4/6) also finish immediately for completed I/O.
            if (args[0]!=0 && args[0]!=1 && args[0]!=3 && args[0]!=4 &&
                args[0]!=5 && args[0]!=6 && args[0]!=16 && args[0]!=17) return {};
            return returned(0);
        case 12:
            if (!volume) return returned(0);
            if (media_()==1) return returned(0x12);
            if (media_()==2) return returned(0x14);
            return {};
        case 13:
            if (args[0]==8) return returned(volume ? 0x40U : 1U);
            if (args[0]>1) return {};
            return returned(volume ? 2U : 6U);
        case 14: {
            // The mounted ISO has no ejectable tray and cannot be swapped by
            // this API. Querying it reports zero media changes, not a fake eject.
            if (args[0]!=2) return {};
            const auto checked=output(ram,args[1],4);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            store_native_word(ram,*ram_offset(args[1],4,ram.size()),0);
            return returned(1);
        }
        case 15:
            if (pending_) return returned(0);
            if (!volume) { error_=0x12; return returned(0); }
            // Reads are synchronous native file operations, so no transfer is
            // outstanding. Retain the public stopped state and completion edge;
            // a later read resumes the mounted volume without optical timing.
            stopped_=true; error_=0;
            if (callback_) pending_=NativeCallback{callback_,6,{}};
            return returned(1);
        case 28: return returned(volume ? (stopped_ ? 0U : 0x0aU) : 0U);
        case 37: {
            if (pending_) return returned(0);
            if (args[0]) {
                const auto target=ram_offset(args[0],4,ram.size());
                if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,args[0]};
            }
            const auto previous=callback_; callback_=args[0]; return returned(previous);
        }
        case 39:
            error_=1;
            pending_.reset();
            if (callback_) pending_=NativeCallback{callback_,8,{}};
            return returned(1);
        case 83: {
            for (unsigned i=0;i<2;++i) {
                const auto checked=output(ram,args[i],4);
                if (checked.status!=NativeServiceStatus::returned) return checked;
            }
            if (!volume) return returned(0);
            std::uint32_t layer=0;
            try { layer=volume->appended_volume_start(); }
            catch (const std::runtime_error&) { return {}; }
            store_native_word(ram,*ram_offset(args[0],4,ram.size()),layer!=0);
            store_native_word(ram,*ram_offset(args[1],4,ram.size()),layer);
            return returned(1);
        }
        default: return {};
        }
    }
    // sceCdSearchFile: fills {sector, size, name[16], date[8]} from the mounted
    // volume. A missing path returns 0, as on the console; dates are not retained.
    NativeServiceReply search(const NativeServiceRequest& request,std::span<std::uint8_t> ram,disc::Iso9660* volume) {
        constexpr std::uint32_t record_size=32;
        const auto& args=request.arguments;
        const auto checked=output(ram,args[0],record_size);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto text=ram_offset(args[1],1,ram.size());
        if (!text) return {NativeServiceStatus::address_fault,0,args[1]};
        const auto limit=std::min<std::size_t>(ram.size()-*text,256);
        const auto first=ram.begin()+*text,end=std::find(first,first+limit,0);
        if (end==first+limit) return {NativeServiceStatus::address_fault,0,args[1]};
        if (!volume) { error_=0x12; return {NativeServiceStatus::returned,0}; }
        const auto record=*ram_offset(args[0],4,ram.size());
        try {
            const auto entry=volume->find(std::string(first,end));
            if (entry.name.size()>15) return {};
            std::fill_n(ram.begin()+record,record_size,std::uint8_t{0});
            store_native_word(ram,record,entry.sector);
            store_native_word(ram,record+4,entry.size);
            std::copy(entry.name.begin(),entry.name.end(),ram.begin()+record+8);
        } catch (const disc::PathError&) { return {NativeServiceStatus::returned,0}; }
        return {NativeServiceStatus::returned,1};
    }
    NativeServiceReply read(const NativeServiceRequest& request,std::span<std::uint8_t> ram,disc::Iso9660* volume) {
        const auto& args=request.arguments;
        if (pending_) return {NativeServiceStatus::returned,0};
        const auto mode=ram_offset(args[3],1,ram.size());
        if (!mode || 4>ram.size()-*mode || 4>0x200000-*mode)
            return {NativeServiceStatus::address_fault,0,args[3]};
        // Sector images contain only 2048-byte user sectors. Other patterns
        // require data that is not present and remain explicit boundaries.
        if (ram[*mode+2]!=0 || ram[*mode+1]>6) return {};
        if (!volume) { error_=0x12; return {NativeServiceStatus::returned,0}; }
        if (!args[1] || args[1]>0x200000/2048) { error_=0x21; return {NativeServiceStatus::returned,0}; }
        const auto bytes=args[1]*2048;
        const auto checked=output(ram,args[2],bytes);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        auto first=args[0];
        if (media_()==2) {
            std::uint32_t layer;
            try { layer=volume->appended_volume_start(); }
            catch (const std::runtime_error&) { return {}; }
            // CDVDMAN callers address each volume with its 16-sector prefix.
            // A joined DVD image starts the appended volume at its descriptor,
            // so translate the public LSN into the file's sector offset.
            if (layer && first>=layer) {
                if (layer<16) return {};
                first-=16;
            }
        }
        if (first>volume->sector_count() || args[1]>volume->sector_count()-first) {
            error_=0x20; return {NativeServiceStatus::returned,0};
        }
        try {
            stopped_=false;
            const auto data=volume->read_sectors(first,args[1]);
            std::copy(data.begin(),data.end(),ram.begin()+*ram_offset(args[2],4,ram.size()));
            error_=0;
        } catch (const std::runtime_error&) { error_=0x30; }
        if (callback_) pending_=NativeCallback{callback_,1,{}};
        return {NativeServiceStatus::returned,1};
    }
    static NativeServiceReply output(std::span<std::uint8_t> ram,std::uint32_t pointer,std::uint32_t size) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,pointer};
        return {NativeServiceStatus::returned};
    }
    Clock clock_;
    Volume volume_;
    std::function<std::uint32_t()> media_;
    std::uint32_t callback_=0,error_=0;
    std::optional<NativeCallback> pending_;
    std::optional<NativeCallback> power_callback_,power_pending_;
    bool application_exit_enabled_=false,close_requested_=false,exit_requested_=false;
    bool active_=false,stopped_=false;
};
}
