#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/iso9660.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>

namespace nt::ee {
// The public CD/DVD initialization endpoint owns a native ISO session. Other
// CD/DVD endpoints are registered separately only when implemented.
class NativeCdvdInitRpc final : public NativeRpcService {
public:
    explicit NativeCdvdInitRpc(std::filesystem::path image,std::uint32_t server=0x4000,std::uint32_t buffer=0x5000,
                              std::uint32_t mounted_media=0)
        : NativeRpcService(0x80000592U,server,buffer),image_(std::move(image)),mounted_media_(mounted_media) {
        if (mounted_media>2) throw std::invalid_argument("Invalid native optical media kind");
    }
    bool initialized() const { return bool(disc_); }
    disc::Iso9660* volume() const { return disc_.get(); }
    std::uint32_t read_error() const { return read_error_; }
    void complete_read(std::uint32_t error) { read_error_=error; }
    // Public media modes are CD (1) and DVD (2). Zero means the session has not selected one.
    std::uint32_t media_mode() const { return media_mode_; }
    // A raw ISO has no reliable physical media identity. The mount declares
    // CD/DVD explicitly; access-mode selection does not change that identity.
    std::optional<std::uint32_t> disk_type() const {
        if (!disc_) return 0;
        if (mounted_media_==1) return 0x12;
        if (mounted_media_==2) return 0x14;
        return {};
    }
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
        read_error_=0;
        if (!disc_) media_mode_=0;
        error_.clear();
        return Stop::budget;
    }
private:
    std::filesystem::path image_;
    std::unique_ptr<disc::Iso9660> disc_;
    std::uint32_t media_mode_=0,mounted_media_=0,read_error_=0;
    std::string error_;
};

// Public file-search RPC publishes the ISO directory record before its RPC
// completion. The path is data, never a source of native executable addresses.
class NativeCdvdSearchRpc final : public NativeRpcService {
public:
    NativeCdvdSearchRpc(NativeCdvdInitRpc& session,std::uint32_t server,std::uint32_t buffer)
        : NativeRpcService(0x80000597U,server,buffer),session_(session) {}
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        if (method!=0) return Stop::unsupported_system;
        if ((args.size()!=292 && args.size()!=296 && args.size()!=300) || size!=4) return Stop::invalid_state;
        const unsigned record_size=args.size()==292 ? 32 : 36;
        if (args.size()==300 && le32(args,296)!=0) return Stop::unsupported_system;
        const auto path=args.subspan(record_size,256);
        const auto end=std::find(path.begin(),path.end(),0);
        if (end==path.end() || end==path.begin()) return Stop::invalid_state;
        const std::string name(path.begin(),end);
        const auto record_destination=le32(args,record_size+256);
        for (const auto [address,bytes]:{std::pair{destination,4U},std::pair{record_destination,record_size}}) {
            const auto checked=rpc_output(memory,protection,address,bytes);
            if (checked!=Stop::budget) return checked;
        }
        if (!session_.volume()) return Stop::unsupported_system;
        // Preserve the caller's record on a failed search, as the public API
        // leaves its output unspecified then. Snapshot before any aliased write.
        std::array<std::uint8_t,36> record{};
        std::copy_n(args.begin(),record_size,record.begin());
        std::uint32_t found=0;
        try {
            const auto entry=session_.volume()->find(name);
            if (entry.name.size()>15) return Stop::unsupported_system;
            record.fill(0);
            std::memcpy(record.data(),&entry.sector,4); std::memcpy(record.data()+4,&entry.size,4);
            std::copy(entry.name.begin(),entry.name.end(),record.begin()+8);
            const auto& d=entry.recorded; const auto year=1900U+d[0];
            const std::array<std::uint8_t,8> date{entry.flags,d[5],d[4],d[3],d[2],d[1],
                static_cast<std::uint8_t>(year),static_cast<std::uint8_t>(year>>8)};
            std::copy(date.begin(),date.end(),record.begin()+24);
            if (record_size==36) record[32]=entry.flags;
            found=1;
        } catch (const disc::PathError&) {
            found=0;
        } catch (const std::runtime_error&) { return Stop::unsupported_system; }
        std::memcpy(memory.data+runtime::default_ram_address(record_destination),record.data(),record_size);
        std::memcpy(memory.data+runtime::default_ram_address(destination),&found,4);
        return Stop::budget;
    }
private:
    NativeCdvdInitRpc& session_;
};

// Native sector reads publish data and edge-fixup metadata before the existing
// RPC completion invokes the compiled EE callback. No optical transfer loop.
class NativeCdvdReadRpc final : public NativeRpcService {
public:
    NativeCdvdReadRpc(NativeCdvdInitRpc& session,std::uint32_t server,std::uint32_t buffer)
        : NativeRpcService(0x80000595U,server,buffer),session_(session) {}
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        if (method==14) {
            if (!args.empty() || size!=4) return Stop::invalid_state;
            const auto checked=rpc_output(memory,protection,destination,size);
            if (checked!=Stop::budget) return checked;
            const std::uint32_t ready=session_.initialized() ? 2 : 6;
            std::memcpy(memory.data+runtime::default_ram_address(destination),&ready,4);
            return Stop::budget;
        }
        if (method==9) return stream(memory,protection,args,destination,size);
        if (method!=1) return Stop::unsupported_system;
        if (args.size()!=24 || (size!=0 && size!=4)) return Stop::invalid_state;
        auto sector=le32(args,0);
        const auto count=le32(args,4),data_address=le32(args,8),mode=le32(args,12);
        const auto fixup=le32(args,16),position=le32(args,20);
        if ((mode>>16)!=0 || ((mode>>8)&255)>6) return Stop::unsupported_system;
        if (!count || count>0x2000000/2048) return Stop::invalid_state;
        const auto bytes=count*2048;
        for (const auto [address,length]:{std::pair{data_address,bytes},std::pair{fixup,16U},
                std::pair{position,4U},std::pair{destination,size}}) {
            if (!length) continue;
            const auto checked=rpc_output(memory,protection,address,length);
            if (checked!=Stop::budget) return checked;
        }
        std::uint32_t error=0,transferred=0;
        nt::Bytes data;
        auto* volume=session_.volume();
        if (!volume) error=0x12;
        else {
            try {
                if (session_.media_mode()==2) {
                    const auto layer=volume->appended_volume_start();
                    if (layer && sector>=layer) {
                        if (layer<16) return Stop::unsupported_system;
                        sector-=16;
                    }
                }
                if (sector>volume->sector_count() || count>volume->sector_count()-sector) error=0x20;
                else { data=volume->read_sectors(sector,count); transferred=bytes; }
            } catch (const std::runtime_error&) { error=0x30; }
        }
        if (!data.empty()) std::memcpy(memory.data+runtime::default_ram_address(data_address),data.data(),data.size());
        // Direct native copies also handle unaligned buffers. Zero edge lengths
        // tell both public callback layouts there is no deferred fragment to copy.
        std::memset(memory.data+runtime::default_ram_address(fixup),0,16);
        std::memcpy(memory.data+runtime::default_ram_address(position),&transferred,4);
        if (size) std::memcpy(memory.data+runtime::default_ram_address(destination),&transferred,4);
        session_.complete_read(error);
        return Stop::budget;
    }
private:
    // Stream reads complete directly from the ISO, so there is no drive ring
    // buffer to fill: the stream position is the only state, and a status query
    // reports the requested buffer as full.
    Stop stream(Memory& memory,CodeOverlap protection,std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) {
        if (args.size()!=20 || size!=4) return Stop::invalid_state;
        const auto checked=rpc_output(memory,protection,destination,size);
        if (checked!=Stop::budget) return checked;
        const auto first=le32(args,0),count=le32(args,4),target=le32(args,8),command=le32(args,12);
        std::uint32_t result=1;
        switch (command) {
        case 1: case 4: case 9: stream_position_=first; break; // Start, seek and fast seek.
        case 3: case 7: case 8: break;                         // Stop, pause and resume hold no reads.
        case 5: stream_sectors_=first; break;                  // Initialize with the buffer size.
        case 6: result=stream_sectors_; break;                 // Status.
        case 2: {                                              // Read: sectors, with an error in the high half.
            if (!count || count>0x2000000/2048) return Stop::invalid_state;
            const auto output=rpc_output(memory,protection,target,count*2048);
            if (output!=Stop::budget) return output;
            std::uint32_t error=0,transferred=0;
            auto* volume=session_.volume();
            if (!volume) error=0x12;
            else if (stream_position_>volume->sector_count() || count>volume->sector_count()-stream_position_) error=0x20;
            else {
                try {
                    const auto data=volume->read_sectors(stream_position_,count);
                    std::memcpy(memory.data+runtime::default_ram_address(target),data.data(),data.size());
                    transferred=count; stream_position_+=count;
                } catch (const std::runtime_error&) { error=0x30; }
            }
            result=transferred|(error<<16);
            break;
        }
        default: return Stop::unsupported_system;
        }
        std::memcpy(memory.data+runtime::default_ram_address(destination),&result,4);
        return Stop::budget;
    }
    NativeCdvdInitRpc& session_;
    std::uint32_t stream_position_=0,stream_sectors_=0;
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
        if (method==3 || method==4 || method==12) {
            if (!args.empty() || size!=4) return Stop::invalid_state;
            const auto value=method==3 ? session_.disk_type() : std::optional<std::uint32_t>{
                method==4 ? session_.read_error() : session_.initialized() ? 0x0aU : 0U};
            if (!value) return Stop::unsupported_system;
            const auto valid=rpc_output(memory,protection,destination,size);
            if (valid!=Stop::budget) return valid;
            std::memcpy(memory.data+runtime::default_ram_address(destination),&*value,4);
            return Stop::budget;
        }
        if (method==34) return select_media_mode(memory,protection,args,destination,size);
        if (method!=1) return Stop::unsupported_system;
        if (!args.empty() || size!=16 || !clock_) return Stop::invalid_state;
        // The RTC belongs to the host platform, not to mounted optical media.
        // SDK libc startup queries it before opening a CD/DVD session.
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
