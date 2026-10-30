#pragma once
#include <nt/ee_native.hpp>
#include <array>
#include <span>
#include <cstring>
#include <vector>

namespace nt::ee {
struct NativeUploadView { unsigned destination; std::span<const std::uint8_t> bytes; };
struct NativeLaunchView { unsigned entry,top,itop; };
struct NativeVectorDataView { unsigned destination; std::span<const std::uint8_t> bytes; };
// Native packet configuration. Unsupported work is rejected as a whole batch.
class NativePacketSetup {
public:
    // Atomic batch acceptance by a precompiled-program owner. A failed callback
    // must leave that owner unchanged; packet configuration commits only after it.
    void* compiled_context=nullptr;
    Stop (*accept_uploads)(void*,unsigned,std::span<const NativeUploadView>)=nullptr;
    void* launch_context=nullptr;
    Stop (*submit_compiled)(void*,unsigned,std::span<const NativeVectorDataView>,const NativeLaunchView*)=nullptr;
    // Optional conversion diagnostic, never an execution fallback. The payload
    // view is valid only during the callback, and the batch still fails.
    void* upload_context=nullptr;
    void (*uncompiled_upload)(void*,unsigned,unsigned,std::span<const std::uint8_t>)=nullptr;
    void (*uncompiled_packet)(void*,unsigned,std::span<const std::uint8_t>)=nullptr;
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
        case 0: return {Stop::budget,(unit.stopped ? 0x100U : 0U)|(unit.broken ? 0x200U : 0U)|(unit.double_buffer ? 0x80U : 0U)};
        case 2: return {Stop::budget,unit.error_mask};
        case 3: return {Stop::budget,unit.mark};
        case 4: return {Stop::budget,unit.cycle};
        case 5: return {Stop::budget,unit.mode};
        case 6: return {Stop::budget,unit.code};
        case 7: return {Stop::budget,unit.mask};
        case 8: return {Stop::budget,unit.itops};
        case 9: return {Stop::budget,unit.base};
        case 10: return {Stop::budget,unit.offset};
        case 11: return {Stop::budget,unit.tops};
        case 12: return {Stop::budget,unit.itop};
        case 13: return {Stop::budget,unit.top};
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
    unsigned rejected_command() const { return rejected_command_; }
    Stop consume(unsigned channel,std::span<const std::uint8_t> bytes,bool vector_active) {
        if (channel>=2 || bytes.size()%4) return Stop::unsupported_system;
        if (units_[channel].stopped || units_[channel].broken || vector_active) return Stop::unsupported_system;
        auto next=units_[channel];
        std::vector<NativeUploadView> uploads;
        std::vector<NativeVectorDataView> data;
        bool launch=false;
        unsigned entry=0;
        std::size_t offset=0;
        const auto word=[&]() {
            std::uint32_t value; std::memcpy(&value,bytes.data()+offset,4); offset+=4; return value;
        };
        while (offset<bytes.size()) {
            const auto code=word(),command=(code>>24)&127U;
            rejected_command_=command;
            if (code&0x80000000U) return Stop::unsupported_system;
            // Only padding may follow a launch in this atomic submission path.
            if (launch && command) return Stop::unsupported_system;
            switch (command) {
            case 0: break; // Padding/NOP.
            case 1: next.cycle=code&0xffffU; break;
            case 2:
                if (!channel) return Stop::unsupported_system;
                next.offset=code&1023U; next.tops=next.base; next.double_buffer=false; break;
            case 3:
                if (!channel) return Stop::unsupported_system;
                next.base=code&1023U; break;
            case 4: next.itops=code&(channel ? 1023U : 255U); break;
            case 5: next.mode=code&3U; break;
            case 7: next.mark=code&0xffffU; break;
            case 0x10: break; // FLUSHE: no outstanding compiled vector work.
            case 0x14:
                if (!submit_compiled || !uploads.empty()) return Stop::unsupported_vector;
                entry=code&(channel ? 2047U : 511U);
                next.itop=next.itops;
                next.top=next.tops&1023U;
                next.tops=next.base+(next.double_buffer ? 0 : next.offset);
                next.double_buffer=!next.double_buffer;
                launch=true;
                break;
            case 0x20:
                if (bytes.size()-offset<4) return Stop::invalid_state;
                next.mask=word(); break;
            case 0x4a: {
                if (!data.empty()) return Stop::unsupported_vector;
                const auto encoded=(code>>16)&255U,count=encoded ? encoded : 256U;
                const auto size=count*8U;
                if (bytes.size()-offset<size) return Stop::invalid_state;
                if (accept_uploads) {
                    uploads.push_back({code&0xffffU,bytes.subspan(offset,size)});
                    offset+=size;
                    break;
                }
                if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                if (uncompiled_upload) uncompiled_upload(upload_context,channel,code&0xffffU,bytes.subspan(offset,size));
                return Stop::unsupported_system;
            }
            case 0x30: case 0x31:
                if (bytes.size()-offset<16) return Stop::invalid_state;
                for (unsigned lane=0;lane<4;++lane) next.row_column[(command==0x31 ? 4 : 0)+lane]=word();
                break;
            case 0x6c: {
                // Unmasked V4-32, contiguous writes without row arithmetic.
                if (!submit_compiled || !uploads.empty() || next.mode ||
                    (next.cycle&255U)!=((next.cycle>>8)&255U)) return Stop::unsupported_vector;
                const auto count=(code>>16)&255U;
                const auto size=(count ? count : 256U)*16U;
                if (bytes.size()-offset<size) return Stop::invalid_state;
                const auto destination=((code&1023U)+((channel && (code&0x8000U)) ? next.tops : 0U))&
                    (channel ? 1023U : 255U);
                data.push_back({destination,bytes.subspan(offset,size)});
                offset+=size;
                break;
            }
            default: return Stop::unsupported_system;
            }
            next.code=code;
        }
        if (!uploads.empty()) {
            const auto result=accept_uploads(compiled_context,channel,uploads);
            if (result!=Stop::budget) {
                rejected_command_=0x4a;
                if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                if (uncompiled_upload) uncompiled_upload(upload_context,channel,uploads.front().destination,uploads.front().bytes);
                return result;
            }
        }
        if (launch || !data.empty()) {
            const NativeLaunchView request{entry,next.top,next.itop};
            const auto result=submit_compiled(launch_context,channel,data,launch ? &request : nullptr);
            if (result!=Stop::budget) { rejected_command_=launch ? 0x14 : 0x6c; return result; }
        }
        units_[channel]=next; rejected_command_=0;
        return Stop::budget;
    }
private:
    unsigned rejected_command_=0;
    static bool decode(std::uint32_t address,unsigned& channel,unsigned& field) {
        if (address<0x10003800 || address>=0x10004000 || address%16) return false;
        channel=(address-0x10003800)/0x400; field=(address&0x3ff)/16;
        return field<24 && field!=15 && (channel || (field!=10 && field!=11 && field!=12 && field!=14));
    }
    struct Unit {
        bool stopped=false,broken=false;
        bool double_buffer=false;
        unsigned base=0,offset=0,tops=0,top=0,itops=0,itop=0;
        std::uint32_t error_mask=0,mark=0,mode=0,mask=0,cycle=0,code=0;
        std::array<std::uint32_t,8> row_column{};
    };
    std::array<Unit,2> units_{};
    bool gif_paused_=false;
    std::uint32_t gif_mode_=0;
};
}
