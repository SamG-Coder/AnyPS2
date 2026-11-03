#pragma once
#include <nt/ee_native.hpp>
#include <nt/ee_vector_data.hpp>
#include <nt/ee_packet_batch.hpp>
#include <nt/graphics_packet.hpp>
#include <array>
#include <span>
#include <cstring>
#include <optional>
#include <vector>

namespace nt::ee {
struct NativeUploadView { unsigned destination; std::span<const std::uint8_t> bytes; };
struct NativeLaunchView { unsigned entry,top,itop; bool continuation=false; };
struct NativeSubmissionStep {
    std::vector<NativeUploadView> uploads;
    std::vector<NativeVectorDataView> data;
    std::vector<graphics::Packet> drawing;
    std::optional<NativeLaunchView> launch;
};
// Native packet configuration. Each segment ending at a callback boundary is
// atomic; a suspended remainder is owned until the compiled handler releases it.
class NativePacketSetup {
public:
    graphics::BatchConsumer graphics;
    // Atomic batch acceptance by a precompiled-program owner. A failed callback
    // must leave that owner unchanged; packet configuration commits only after it.
    void* compiled_context=nullptr;
    Stop (*accept_uploads)(void*,unsigned,std::span<const NativeUploadView>)=nullptr;
    void* launch_context=nullptr;
    Stop (*submit_compiled)(void*,unsigned,std::span<const NativeVectorDataView>,const NativeLaunchView*)=nullptr;
    Stop (*submit_uploaded)(void*,unsigned,std::span<const NativeUploadView>,
        std::span<const NativeVectorDataView>,const NativeLaunchView*)=nullptr;
    Stop (*submit_combined)(void*,unsigned,std::span<const NativeUploadView>,
        std::span<const NativeVectorDataView>,const NativeLaunchView*,std::span<const graphics::Packet>)=nullptr;
    Stop (*submit_sequence)(void*,unsigned,std::span<const NativeSubmissionStep>)=nullptr;
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
            case 0x20:
                if (gif_stream_.pending()) return {Stop::unsupported_system};
                return {Stop::budget,gif_mode_|(gif_paused_ ? 8U : 0U)|(units_[1].path3_masked ? 2U : 0U)};
            case 0x40: case 0x50: case 0x60: case 0x70: case 0x80: case 0x90: case 0xa0:
                return gif_stream_.pending() ? IoRead{Stop::unsupported_system} : IoRead{Stop::budget,0};
            default: return {Stop::unsupported_system};
            }
        }
        unsigned channel,field;
        if (!decode(address,channel,field)) return {Stop::unsupported_system};
        const auto& unit=units_[channel];
        switch (field) {
        case 0: return {Stop::budget,(unit.stopped ? 0x100U : 0U)|(unit.broken ? 0x200U : 0U)|
            (unit.interrupt_stall ? 0x400U : 0U)|(unit.interrupt ? 0x800U : 0U)|(unit.double_buffer ? 0x80U : 0U)};
        case 2: return {Stop::budget,unit.error_mask};
        case 3: return {Stop::budget,unit.mark};
        case 4: return {Stop::budget,unit.cycle};
        case 5: return {Stop::budget,unit.mode};
        case 6: return {Stop::budget,0}; // No partially consumed UNPACK/MPG command.
        case 7: return {Stop::budget,unit.mask};
        case 8: return {Stop::budget,unit.code};
        case 9: return {Stop::budget,unit.itops};
        case 10: return {Stop::budget,unit.base};
        case 11: return {Stop::budget,unit.offset};
        case 12: return {Stop::budget,unit.tops};
        case 13: return {Stop::budget,unit.itop};
        case 14: return {Stop::budget,unit.top};
        default: return {Stop::budget,field>=16 ? unit.row_column[field-16] : 0};
        }
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address%16) return Stop::unsupported_system;
        if (address==0x10003000) {
            if (value&1) { gif_paused_=false; gif_mode_=0; gif_stream_={}; }
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
            if (value&8) unit.broken=unit.stopped=unit.interrupt=unit.interrupt_stall=false;
            if (value&1) {
                unit.broken=unit.stopped=unit.interrupt=unit.interrupt_stall=false;
                unit.error_mask=0; pending_[channel]={}; interrupt_events_&=~(1U<<channel);
            }
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
    bool pending(unsigned channel) const { return channel<2 && !pending_[channel].bytes.empty(); }
    unsigned take_interrupts() { const auto result=interrupt_events_; interrupt_events_=0; return result; }
    Stop consume(unsigned channel,std::span<const std::uint8_t> bytes,bool vector_active,std::span<const NativePacketOrigin> origins={}) {
        std::size_t consumed=0;
        if (channel>=2) return consume_segment(channel,bytes,vector_active,consumed);
        if (pending(channel)) return Stop::unsupported_system;
        if (bytes.size()>NativePacketBatch::maximum_bytes) return Stop::invalid_state;
        for (std::size_t i=0;i<origins.size();++i)
            if (origins[i].offset>=bytes.size() || origins[i].offset%4 || origins[i].quadword_offset>=16 || origins[i].quadword_offset%4 ||
                (i ? origins[i].offset<=origins[i-1].offset : origins[i].offset!=0)) return Stop::invalid_state;
        // Retain source data independently of subsequent caller writes. No
        // command is re-executed when an interrupt handler releases the suffix.
        Pending candidate{{bytes.begin(),bytes.end()},0,{origins.begin(),origins.end()}};
        const auto result=consume_segment(channel,candidate.bytes,vector_active,consumed,candidate.origins);
        if (result!=Stop::budget) return result;
        if (consumed<candidate.bytes.size()) {
            candidate.offset=consumed; pending_[channel]=std::move(candidate);
        }
        return Stop::budget;
    }
    Stop resume(unsigned channel,bool vector_active) {
        if (!pending(channel)) return Stop::budget;
        if (units_[channel].stopped || units_[channel].broken || units_[channel].interrupt_stall)
            return Stop::budget;
        auto& packet=pending_[channel];
        std::size_t consumed=0;
        const auto result=consume_segment(channel,std::span(packet.bytes).subspan(packet.offset),vector_active,consumed,packet.origins,packet.offset);
        if (result!=Stop::budget) return result;
        packet.offset+=consumed;
        if (packet.offset==packet.bytes.size()) packet={};
        return Stop::budget;
    }
private:
    Stop consume_segment(unsigned channel,std::span<const std::uint8_t> bytes,bool vector_active,std::size_t& consumed,
                         std::span<const NativePacketOrigin> origins={},std::size_t source_offset=0) {
        if (channel==2) {
            if (gif_paused_ || units_[1].path3_masked || !graphics.submit) return Stop::unsupported_system;
            // Descriptor-only submissions carry no GIF data. They complete
            // without inventing a packet or disturbing a partial stream.
            if (bytes.empty()) return Stop::budget;
            std::vector<graphics::Packet> packets;
            auto next=gif_stream_;
            try { next.append(bytes,packets); }
            catch (const std::invalid_argument&) { return Stop::unsupported_system; }
            if (!packets.empty() && !graphics.submit(graphics.context,packets)) return Stop::unsupported_system;
            gif_stream_=std::move(next);
            return Stop::budget;
        }
        if (channel==1 && gif_stream_.pending()) return Stop::unsupported_system;
        if (channel>=2 || bytes.size()%4) return Stop::unsupported_system;
        if (units_[channel].stopped || units_[channel].broken || units_[channel].interrupt_stall || vector_active)
            return Stop::unsupported_system;
        auto next=units_[channel];
        std::vector<NativeUploadView> uploads;
        std::vector<NativeVectorDataView> data;
        std::vector<NativeVectorPlan> data_plans;
        std::vector<graphics::Packet> drawing;
        std::vector<NativeSubmissionStep> sequence;
        std::size_t drawing_bytes=0;
        bool launch=false,continuation=false;
        bool notify=false;
        unsigned entry=0,launch_command=0,wait_command=0;
        std::size_t offset=0;
        const auto word=[&]() {
            std::uint32_t value; std::memcpy(&value,bytes.data()+offset,4); offset+=4; return value;
        };
        const auto finish_step=[&]() {
            NativeSubmissionStep step{std::move(uploads),std::move(data),std::move(drawing),{}};
            if (launch) step.launch=NativeLaunchView{entry,next.top,next.itop,continuation};
            sequence.push_back(std::move(step));
            uploads.clear(); data.clear(); drawing.clear(); launch=false;
        };
        while (offset<bytes.size()) {
            const auto code=word(),command=(code>>24)&127U;
            rejected_command_=command;
            const bool interrupt=(code&0x80000000U) && !(next.error_mask&1U);
            if (launch && command) {
                if (!submit_sequence || sequence.size()+1>=NativePacketBatch::maximum_descriptors) return Stop::unsupported_system;
                finish_step();
            }
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
            case 6:
                if (!channel) return Stop::unsupported_system;
                next.path3_masked=(code&0x8000U)!=0; break;
            case 7: next.mark=code&0xffffU; break;
            case 0x10: break; // FLUSHE: no outstanding compiled vector work.
            case 0x11: case 0x13:
                if (!channel || !graphics.wait || (!drawing.empty() && !graphics.synchronous)) return Stop::unsupported_system;
                wait_command=command;
                break;
            case 0x14: case 0x15: case 0x17:
                if (!submit_compiled || (!uploads.empty() && !submit_uploaded) || (!drawing.empty() && !submit_combined)) return Stop::unsupported_vector;
                if (command==0x15) {
                    if (!channel || !graphics.wait) return Stop::unsupported_system;
                    wait_command=command;
                }
                launch_command=command;
                continuation=command==0x17;
                entry=code&(channel ? 2047U : 511U);
                next.itop=next.itops;
                if (channel) {
                    next.top=next.tops&1023U;
                    next.tops=next.base+(next.double_buffer ? 0 : next.offset);
                    next.double_buffer=!next.double_buffer;
                }
                launch=true;
                break;
            case 0x20:
                if (bytes.size()-offset<4) return Stop::invalid_state;
                next.mask=word(); break;
            case 0x4a: {
                if ((!data.empty() && !submit_uploaded) || (!drawing.empty() && !submit_combined)) return Stop::unsupported_vector;
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
            case 0x60: case 0x61: case 0x62:
            case 0x64: case 0x65: case 0x66:
            case 0x68: case 0x69: case 0x6a:
            case 0x70: case 0x71: case 0x72:
            case 0x74: case 0x75: case 0x76:
            case 0x78: case 0x79: case 0x7a:
            case 0x7c: case 0x7d: case 0x7e: case 0x7f:
            case 0x6c: case 0x6d: case 0x6e: case 0x6f: {
                // Typed vector data, retaining source alignment and bounded lookahead.
                const auto format=static_cast<NativeVectorFormat>(command&3U);
                const auto components=((command>>2)&3U)+1;
                const auto raw_span=next.cycle&255U,raw_writes=(next.cycle>>8)&255U;
                if (!submit_compiled || (!uploads.empty() && !submit_uploaded) || (!drawing.empty() && !submit_combined) ||
                    (!raw_span && raw_writes) || (!raw_writes && raw_span)) return Stop::unsupported_vector;
                const auto encoded_count=(code>>16)&255U,count=encoded_count ? encoded_count : 256U;
                const auto span=raw_span ? raw_span : 256U,writes=raw_writes ? raw_writes : 256U;
                const auto inputs=writes>span ? (count/writes)*span+std::min(count%writes,span) : count;
                const auto size=inputs*vector_data_stride(format,components);
                const auto padded=(size+3U)&~3U;
                if (bytes.size()-offset<padded) return Stop::invalid_state;
                const auto destination=((code&1023U)+((channel && (code&0x8000U)) ? next.tops : 0U))&
                    (channel ? 1023U : 255U);
                data.push_back({destination,bytes.subspan(offset,size),format,(code&0x4000U)!=0,components});
                auto& view=data.back();
                const auto command_position=source_offset+offset-4;
                const auto origin=std::upper_bound(origins.begin(),origins.end(),command_position,
                    [](std::size_t position,const NativePacketOrigin& item) { return position<item.offset; });
                const auto aligned=origin==origins.begin() ? command_position :
                    command_position-(origin-1)->offset+(origin-1)->quadword_offset;
                view.source_alignment=unsigned((aligned/4)%4)+1;
                view.following_bytes=bytes.subspan(offset+size);
                view.destination_cycle=span;
                view.writes_per_cycle=writes;
                view.output_count=count;
                view.mode=next.mode;
                std::copy_n(next.row_column.begin(),4,view.rows.begin());
                if (command&16U) {
                    const auto cycle=(next.cycle>>8)&255U;
                    data.back().mask=NativeVectorMask{next.mask,next.row_column,cycle ? cycle : 256U};
                }
                auto plan=plan_vector_data(channel ? 1024 : 256,view);
                if (!plan) return Stop::unsupported_vector;
                std::copy(plan->rows.begin(),plan->rows.end(),next.row_column.begin());
                data_plans.push_back(std::move(*plan));
                view.planned=data_plans.back().writes;
                offset+=padded;
                break;
            }
            case 0x50: case 0x51: {
                if (!channel || gif_paused_ || !graphics.submit || ((!uploads.empty() || !data.empty()) && !submit_combined)) return Stop::unsupported_system;
                const auto count=code&65535U,size=(count ? count : 65536U)*16U;
                if (size>bytes.size()-offset) return Stop::invalid_state;
                if (size>65536U*16U-drawing_bytes) return Stop::unsupported_system;
                try { graphics::append_packets(drawing,bytes.subspan(offset,size)); }
                catch (const std::invalid_argument&) { return Stop::unsupported_system; }
                drawing_bytes+=size; offset+=size;
                break;
            }
            default: return Stop::unsupported_system;
            }
            next.code=code;
            if (interrupt) {
                notify=next.interrupt=true;
                if (command!=7) { next.interrupt_stall=true; break; }
            }
        }
        // Validation precedes the host wait. No current-batch effects have
        // occurred yet, and a retry cannot replay a previously submitted draw.
        // A fence after DIRECT is coalesced only for a synchronous consumer:
        // the final ordered batch completes before any state is published.
        // DIRECT packets precede a final vector launch in a combined transaction.
        if (wait_command && !graphics.wait(graphics.context)) {
            rejected_command_=wait_command;
            return Stop::unsupported_system;
        }
        if (!sequence.empty()) {
            finish_step();
            const auto result=submit_sequence(launch_context,channel,sequence);
            if (result!=Stop::budget) {
                if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                for (const auto& step:sequence) for (const auto& upload:step.uploads)
                    if (uncompiled_upload) uncompiled_upload(upload_context,channel,upload.destination,upload.bytes);
                return result;
            }
            units_[channel]=next; consumed=offset; rejected_command_=0;
            if (notify) interrupt_events_|=1U<<channel;
            return Stop::budget;
        }
        const bool combined=!uploads.empty() && (launch || !data.empty());
        const bool drawing_vectors=!drawing.empty() && (launch || !data.empty() || !uploads.empty());
        if (drawing_vectors) {
            if (!submit_combined) return Stop::unsupported_vector;
            const NativeLaunchView request{entry,next.top,next.itop,continuation};
            const auto result=submit_combined(launch_context,channel,uploads,data,launch ? &request : nullptr,drawing);
            if (result!=Stop::budget) {
                if (!uploads.empty()) {
                    if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                    if (uncompiled_upload) uncompiled_upload(upload_context,channel,uploads.front().destination,uploads.front().bytes);
                }
                return result;
            }
        }
        if (!drawing_vectors && !uploads.empty() && !combined) {
            const auto result=accept_uploads(compiled_context,channel,uploads);
            if (result!=Stop::budget) {
                rejected_command_=0x4a;
                if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                if (uncompiled_upload) uncompiled_upload(upload_context,channel,uploads.front().destination,uploads.front().bytes);
                return result;
            }
        }
        if (!drawing_vectors && (launch || !data.empty())) {
            const NativeLaunchView request{entry,next.top,next.itop,continuation};
            const auto result=combined ? submit_uploaded(launch_context,channel,uploads,data,launch ? &request : nullptr) :
                submit_compiled(launch_context,channel,data,launch ? &request : nullptr);
            if (result!=Stop::budget) {
                rejected_command_=launch ? launch_command : 0x6c;
                if (combined) {
                    if (uncompiled_packet) uncompiled_packet(upload_context,channel,bytes);
                    if (uncompiled_upload) uncompiled_upload(upload_context,channel,uploads.front().destination,uploads.front().bytes);
                }
                return result;
            }
        }
        if (!drawing_vectors && !drawing.empty() && !graphics.submit(graphics.context,drawing)) return Stop::unsupported_system;
        units_[channel]=next; consumed=offset; rejected_command_=0;
        if (notify) interrupt_events_|=1U<<channel;
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
        bool interrupt=false,interrupt_stall=false;
        bool double_buffer=false,path3_masked=false;
        unsigned base=0,offset=0,tops=0,top=0,itops=0,itop=0;
        std::uint32_t error_mask=0,mark=0,mode=0,mask=0,cycle=0,code=0;
        std::array<std::uint32_t,8> row_column{};
    };
    struct Pending { std::vector<std::uint8_t> bytes; std::size_t offset=0; std::vector<NativePacketOrigin> origins; };
    std::array<Unit,2> units_{};
    std::array<Pending,2> pending_{};
    unsigned interrupt_events_=0;
    graphics::PacketStream gif_stream_;
    bool gif_paused_=false;
    std::uint32_t gif_mode_=0;
};
}
