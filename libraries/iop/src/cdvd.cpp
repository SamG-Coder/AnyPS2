#include <nt/iop_cdvd.hpp>

namespace nt::iop {
std::optional<std::uint32_t> Cdvd::read(std::uint32_t address,unsigned bytes) {
    if (bytes!=1) return {};
    switch (address) {
    case 0x1f402004: return normal_command_;
    case 0x1f402005: return ready_;
    case 0x1f402006: { const auto value=error_; error_=0; return value; }
    case 0x1f402007: return 0; // No supported command leaves an active operation.
    case 0x1f402008: return interrupt_;
    case 0x1f40200a: return status_;
    case 0x1f40200b: return sticky_;
    case 0x1f402013: return 0; // Motor stopped.
    case 0x1f402016: return sub_command_;
    case 0x1f402017: return result_position_==result_size_ ? 0x40 : 0;
    case 0x1f402018: return result_position_<result_size_ ? result_[result_position_++] : 0;
    default: return {};
    }
}
bool Cdvd::subcommand(std::uint8_t command) {
    std::array<std::uint8_t,16> reply{};
    unsigned size=0;
    auto status=status_, sticky=sticky_;
    if (command==3 && sub_size_==1 && sub_parameters_[0]==0) {
        for (unsigned i=0;i<mechanical_version_.size();++i) reply[i]=mechanical_version_[i];
        size=static_cast<unsigned>(mechanical_version_.size());
    } else if (command==3 && sub_size_==1 && sub_parameters_[0]==0x30) {
        reply[0]=status; reply[1]=(status&1) ? 8 : 0; size=2;
    } else if (command==5 && sub_size_==0) {
        sticky=status&1; size=1;
    } else if (command==0x1b && sub_size_==0) {
        ready_&=static_cast<std::uint8_t>(~0x20U);
        size=1;
    } else if (command==8 && sub_size_==0 && clock_) {
        using namespace std::chrono;
        // The device clock uses UTC+9; software applies the user's time zone.
        // Latch once so draining the FIFO cannot mix two timestamps.
        const auto time=floor<seconds>(clock_()+hours{9});
        const auto date=floor<days>(time);
        const year_month_day calendar{date};
        const hh_mm_ss clock{time-date};
        const auto bcd=[](unsigned value) { return static_cast<std::uint8_t>((value/10)*16+value%10); };
        reply[1]=bcd(static_cast<unsigned>(clock.seconds().count()));
        reply[2]=bcd(static_cast<unsigned>(clock.minutes().count()));
        reply[3]=bcd(static_cast<unsigned>(clock.hours().count()));
        reply[5]=bcd(static_cast<unsigned>(calendar.day()));
        reply[6]=bcd(static_cast<unsigned>(calendar.month()));
        reply[7]=bcd(static_cast<unsigned>((int(calendar.year())%100+100)%100));
        size=8;
    } else return false;
    status_=status; sticky_=sticky;
    result_=reply; result_size_=size; result_position_=0;
    sub_command_=command; sub_size_=0;
    return true;
}
bool Cdvd::write(std::uint32_t address,unsigned bytes,std::uint32_t value) {
    if (bytes!=1) return false;
    const auto byte=static_cast<std::uint8_t>(value);
    switch (address) {
    case 0x1f402004:
        if (byte!=0) return false; // NOP only; media commands need a backing device.
        normal_command_=byte;
        error_=normal_size_ ? 0x22 : 0;
        ready_=error_ ? 0x41 : 0x40;
        normal_size_=0; interrupt_|=2;
        return true;
    case 0x1f402005:
        if (normal_size_==normal_parameters_.size()) return false;
        normal_parameters_[normal_size_++]=byte; return true;
    case 0x1f402006: how_to_=byte; return true;
    case 0x1f402007: return true; // Idle BREAK has no active operation to abort.
    case 0x1f402008: interrupt_&=static_cast<std::uint8_t>(~byte); return true;
    case 0x1f402016: return subcommand(byte);
    case 0x1f402017:
        if (sub_size_==sub_parameters_.size()) return false;
        sub_parameters_[sub_size_++]=byte; return true;
    case 0x1f40203a: return byte==0; // Enabling a decryption path is unsupported.
    default: return false;
    }
}
}
