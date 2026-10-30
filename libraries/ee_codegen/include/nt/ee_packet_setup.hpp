#pragma once
#include <nt/ee_native.hpp>
#include <array>

namespace nt::ee {
// Native submission configuration only. Packet execution must have a separate
// converted implementation before any input may be accepted or completed.
class NativePacketSetup {
public:
    static bool maps(std::uint32_t address) { return address>=0x10003000 && address<0x10007000; }
    IoRead read(std::uint32_t address) const {
        if (address%16) return {Stop::unsupported_system};
        if (address<0x10003800) {
            switch (address-0x10003000) {
            case 0: return {Stop::budget,gif_paused_ ? 8U : 0U};
            case 0x10: return {Stop::budget,gif_mode_};
            case 0x20: return {Stop::budget,gif_mode_|(gif_paused_ ? 8U : 0U)};
            case 0x40: case 0x50: case 0x60: case 0x70: case 0x80: case 0x90: case 0xa0:
                return {Stop::budget,0}; // No native packet has been accepted.
            default: return {Stop::unsupported_system};
            }
        }
        unsigned channel,field;
        if (!decode(address,channel,field)) return {Stop::unsupported_system};
        const auto& unit=units_[channel];
        switch (field) {
        case 0: return {Stop::budget,(unit.stopped ? 0x100U : 0U)|(unit.broken ? 0x200U : 0U)};
        case 2: return {Stop::budget,unit.error_mask};
        case 3: return {Stop::budget,unit.mark};
        case 5: return {Stop::budget,unit.mode};
        case 7: return {Stop::budget,unit.mask};
        default: return {Stop::budget,field>=16 ? unit.row_column[field-16] : 0};
        }
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address%16) return Stop::unsupported_system;
        if (address==0x10003000) {
            if (value&1) { gif_paused_=false; gif_mode_=0; }
            else gif_paused_=(value&8)!=0;
            return Stop::budget;
        }
        if (address==0x10003010) { gif_mode_=value&5; return Stop::budget; }
        unsigned channel,field;
        if (!decode(address,channel,field)) return Stop::unsupported_system;
        auto& unit=units_[channel];
        switch (field) {
        case 0:
            return channel && (value&0x800000) ? Stop::unsupported_system : Stop::budget;
        case 1:
            if (value&2) unit.broken=true;
            if (value&4) unit.stopped=true;
            if (value&8) unit.broken=unit.stopped=false;
            if (value&1) { unit.broken=unit.stopped=false; unit.error_mask=0; }
            return Stop::budget;
        case 2: unit.error_mask=value&7; return Stop::budget;
        case 3: unit.mark=value&0xffff; return Stop::budget;
        case 5: unit.mode=value&3; return Stop::budget;
        case 7: unit.mask=value; return Stop::budget;
        default:
            if (field>=16) { unit.row_column[field-16]=value; return Stop::budget; }
            return Stop::unsupported_system;
        }
    }
private:
    static bool decode(std::uint32_t address,unsigned& channel,unsigned& field) {
        if (address<0x10003800 || address>=0x10004000 || address%16) return false;
        channel=(address-0x10003800)/0x400; field=(address&0x3ff)/16;
        return field<24 && field!=15 && (channel || (field!=10 && field!=11 && field!=12 && field!=14));
    }
    struct Unit {
        bool stopped=false,broken=false;
        std::uint32_t error_mask=0,mark=0,mode=0,mask=0;
        std::array<std::uint32_t,8> row_column{};
    };
    std::array<Unit,2> units_{};
    bool gif_paused_=false;
    std::uint32_t gif_mode_=0;
};
}
