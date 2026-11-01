#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/iso9660.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>

namespace nt::ee {
// The public CD/DVD initialization endpoint owns a native ISO session. Other
// CD/DVD endpoints are registered separately only when implemented.
class NativeCdvdInitRpc final : public NativeRpcService {
public:
    explicit NativeCdvdInitRpc(std::filesystem::path image,std::uint32_t server=0x4000,std::uint32_t buffer=0x5000)
        : NativeRpcService(0x80000592U,server,buffer),image_(std::move(image)) {}
    bool initialized() const { return bool(disc_); }
    disc::Iso9660* volume() const { return disc_.get(); }
    // Public media modes are CD (1) and DVD (2). Zero means the session has not selected one.
    std::uint32_t media_mode() const { return media_mode_; }
    bool select_media(std::uint32_t mode) {
        if (!disc_ || (mode!=1 && mode!=2)) return false;
        media_mode_=mode;
        return true;
    }
    const std::string& error() const { return error_; }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        if (method!=0) return Stop::unsupported_system;
        if (args.size()!=4 || (size!=4 && size!=16)) return Stop::invalid_state;
        const auto mode=le32(args,0);
        if (mode!=0 && mode!=1 && mode!=5) return Stop::unsupported_system;
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        std::unique_ptr<disc::Iso9660> next;
        try {
            if (mode!=5) {
                next=std::make_unique<disc::Iso9660>(image_);
                // Normal initialization waits for usable media. Library-only
                // initialization opens the volume without traversing its root.
                if (mode==0) next->entries(next->root());
            }
        } catch (const std::runtime_error& e) {
            error_=e.what(); return Stop::unsupported_system;
        }
        // Public legacy response: success followed by absent module-version
        // metadata and disabled verbose logging. No Sony revision is claimed.
        const std::array<std::uint8_t,16> result{1};
        std::memcpy(memory.data+runtime::default_ram_address(destination),result.data(),size);
        disc_=std::move(next);
        if (!disc_) media_mode_=0;
        error_.clear();
        return Stop::budget;
    }
private:
    std::filesystem::path image_;
    std::unique_ptr<disc::Iso9660> disc_;
    std::uint32_t media_mode_=0;
    std::string error_;
};

// Native CD/DVD synchronous commands. ReadClock uses the host clock.
// Media-mode selection stores CD or DVD on the shared ISO session.
// Unknown drive commands remain explicit unsupported boundaries.
class NativeCdvdScmdRpc final : public NativeRpcService {
public:
    using Clock = std::chrono::sys_seconds (*)();
    NativeCdvdScmdRpc(NativeCdvdInitRpc& session,std::uint32_t server,
                      std::uint32_t buffer,Clock clock=wall_clock)
        : NativeRpcService(0x80000593U,server,buffer),session_(session),clock_(clock) {}
    std::uint32_t last_method() const { return last_method_; }
    std::uint32_t last_argument() const { return last_argument_; }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        last_method_=method;
        if (method==34) return select_media_mode(memory,protection,args,destination,size);
        if (method!=1) return Stop::unsupported_system;
        if (!args.empty() || size!=16 || !clock_) return Stop::invalid_state;
        if (!session_.initialized()) return Stop::unsupported_system;
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        using namespace std::chrono;
        // The console clock ABI uses Japan standard time, independently of the
        // PC's configured timezone. Keep calendar conversion in native code.
        const auto now=clock_()+hours{9};
        const auto day=floor<days>(now);
        const year_month_day date{day};
        const hh_mm_ss time{now-day};
        const auto year_value=int(date.year());
        if (year_value<2000 || year_value>2099) return Stop::unsupported_system;
        const auto bcd=[](unsigned value) { return static_cast<std::uint8_t>((value/10)*16+value%10); };
        const std::array<std::uint8_t,16> result{1,0,0,0,0,
            bcd(static_cast<unsigned>(time.seconds().count())),
            bcd(static_cast<unsigned>(time.minutes().count())),
            bcd(static_cast<unsigned>(time.hours().count())),0,
            bcd(unsigned(date.day())),bcd(unsigned(date.month())),bcd(year_value-2000)};
        std::memcpy(memory.data+runtime::default_ram_address(destination),result.data(),result.size());
        return Stop::budget;
    }
private:
    Stop select_media_mode(Memory& memory,CodeOverlap protection,std::span<const std::uint8_t> args,
                           std::uint32_t destination,std::uint32_t size) {
        // Public EE wrapper sends one int and receives one int. A fifth trailing
        // byte is an IOP-side guard for a longer packet, not part of this call.
        if (args.size()!=4 || size!=4) return Stop::invalid_state;
        last_argument_=le32(args,0);
        if (!session_.initialized()) return Stop::unsupported_system;
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        if (!session_.select_media(last_argument_)) return Stop::unsupported_system;
        const std::array<std::uint8_t,4> result{1,0,0,0};
        std::memcpy(memory.data+runtime::default_ram_address(destination),result.data(),result.size());
        return Stop::budget;
    }
    static std::chrono::sys_seconds wall_clock() {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    }
    NativeCdvdInitRpc& session_;
    Clock clock_;
    std::uint32_t last_method_=0;
    std::uint32_t last_argument_=0;
};

// Public disk-ready servers 0x8000059A and 0x8000059C share one callback.
// An open ISO with no transfer in progress reports the idle-ready status.
// Polling a closed session reports not ready. The blocking mode completes
// immediately because this service has no outstanding transfer to wait for.
class NativeCdvdReadyRpc final : public NativeRpcService {
public:
    NativeCdvdReadyRpc(const NativeCdvdInitRpc& session,std::uint32_t id,
                       std::uint32_t server,std::uint32_t buffer)
        : NativeRpcService(id,server,buffer),session_(session) {}
    std::uint32_t calls() const { return calls_; }
    std::uint32_t last_mode() const { return last_mode_; }
    std::uint32_t last_result() const { return last_result_; }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        ++calls_;
        if (method!=0) return Stop::unsupported_system;
        if (args.size()!=4 || size!=4) return Stop::invalid_state;
        last_mode_=le32(args,0);
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        const bool idle=session_.initialized();
        std::uint32_t result=6;
        if (last_mode_==8) result=idle ? 0x40U : 1U;
        else if (last_mode_==0) result=2;
        else if (idle) result=2;
        last_result_=result;
        const std::array<std::uint8_t,4> bytes{
            static_cast<std::uint8_t>(result),
            static_cast<std::uint8_t>(result>>8),
            static_cast<std::uint8_t>(result>>16),
            static_cast<std::uint8_t>(result>>24)};
        std::memcpy(memory.data+runtime::default_ram_address(destination),bytes.data(),bytes.size());
        return Stop::budget;
    }
private:
    const NativeCdvdInitRpc& session_;
    std::uint32_t calls_=0;
    std::uint32_t last_mode_=0;
    std::uint32_t last_result_=0;
};
}
