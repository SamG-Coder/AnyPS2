#include <nt/host_metrics.hpp>
#include <nt/counter_intervals.hpp>
#include <nt/etw_counters.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <evntcons.h>
#include <array>
#include <limits>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        using nt::host::CounterIntervals;
        using nt::host::CounterStatus;
        const auto event=[](CounterIntervals& owner,unsigned cpu,unsigned old_thread,unsigned new_thread,
                            std::uint64_t time,std::uint64_t cycles,std::uint64_t misses) {
            const std::array counters{cycles,misses};
            owner.consume({cpu,old_thread,new_thread,time,counters});
        };
        CounterIntervals intervals(7,2,2);
        event(intervals,0,7,8,1,90,9); // Initial partial interval cannot be measured.
        if (intervals.snapshot().status!=CounterStatus::waiting) throw std::runtime_error("Partial first interval accepted");
        event(intervals,0,8,7,2,100,10);
        event(intervals,1,9,10,3,9000,900); // Other processor's work must not be counted.
        if (intervals.snapshot().status!=CounterStatus::running) throw std::runtime_error("Active interval fabricated a sample");
        event(intervals,0,7,8,4,180,13);
        event(intervals,1,10,7,5,20000,1000); // Migration establishes a new CPU baseline.
        event(intervals,1,7,10,6,20040,1002);
        const auto complete=intervals.snapshot();
        if (complete.status!=CounterStatus::ready || complete.completed_intervals!=2 || complete.end_timestamp!=6 ||
            complete.totals!=std::vector<std::uint64_t>{120,5}) throw std::runtime_error("Native counter thread attribution differs");
        intervals.lost_events();
        event(intervals,0,8,7,7,300,20);
        if (intervals.snapshot().status!=CounterStatus::lost_events || intervals.snapshot().totals!=complete.totals)
            throw std::runtime_error("Lost-event stream resumed with invented coverage");
        CounterIntervals mismatch(7,2,2);
        event(mismatch,0,8,7,1,100,10); event(mismatch,0,9,8,2,200,20);
        if (mismatch.snapshot().status!=CounterStatus::discontinuity) throw std::runtime_error("Missing switch-out accepted");
        CounterIntervals migration(7,2,2);
        event(migration,0,8,7,1,100,10); event(migration,1,9,7,2,200,20);
        if (migration.snapshot().status!=CounterStatus::discontinuity) throw std::runtime_error("Concurrent target execution accepted");
        CounterIntervals regression(7,2,2);
        event(regression,0,8,7,1,100,10); event(regression,0,7,8,2,200,9);
        if (regression.snapshot().status!=CounterStatus::regressed_counter || regression.snapshot().totals[0])
            throw std::runtime_error("Counter reset committed a partial measurement");
        CounterIntervals unordered(7,2,2);
        event(unordered,0,8,7,2,100,10); event(unordered,0,7,8,1,200,20);
        if (unordered.snapshot().status!=CounterStatus::discontinuity) throw std::runtime_error("Out-of-order samples accepted");
        CounterIntervals overflow(7,2,2);
        event(overflow,0,8,7,1,0,0);
        event(overflow,0,7,8,2,std::numeric_limits<std::uint64_t>::max(),0);
        event(overflow,1,8,7,3,0,0); event(overflow,1,7,8,4,1,0);
        if (overflow.snapshot().status!=CounterStatus::overflow) throw std::runtime_error("Counter total overflow accepted");
        CounterIntervals malformed(7,2,2);
        malformed.consume({0,8,7,1,{}});
        if (malformed.snapshot().status!=CounterStatus::invalid_sample) throw std::runtime_error("Missing counters read as zero");
        for (const unsigned version : {2u,5u}) {
            EVENT_RECORD record{};
            record.EventHeader.ProviderId={0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
            record.EventHeader.EventDescriptor.Opcode=36;
            record.EventHeader.EventDescriptor.Version=static_cast<UCHAR>(version);
            record.EventHeader.TimeStamp.QuadPart=1;
            record.EventHeader.Flags=EVENT_HEADER_FLAG_PROCESSOR_INDEX;
            record.BufferContext.ProcessorIndex=300;
            std::array<std::uint32_t,7> payload{7,8,0,0,0,0,0xfedcba98};
            const auto payload_size=static_cast<USHORT>(version==2 ? 24 : 28);
            std::array<std::uint64_t,2> readings{100,10};
            std::array<EVENT_HEADER_EXTENDED_DATA_ITEM,2> extensions{};
            extensions[0].ExtType=EVENT_HEADER_EXT_TYPE_PMC_COUNTERS;
            extensions[0].DataSize=sizeof(readings);
            extensions[0].DataPtr=reinterpret_cast<std::uintptr_t>(readings.data());
            record.UserData=payload.data(); record.UserDataLength=payload_size;
            record.ExtendedData=extensions.data(); record.ExtendedDataCount=1;
            auto decoded=nt::host::decode_counter_switch(record,2);
            if (!decoded || decoded->processor!=300 || decoded->incoming_thread!=7 || decoded->outgoing_thread!=8)
                throw std::runtime_error("ETW thread identity/processor decoding differs");
            CounterIntervals attributed(7,512,2);
            attributed.consume(decoded->view());
            payload[0]=8; payload[1]=7; readings={180,13}; record.EventHeader.TimeStamp.QuadPart=2;
            attributed.consume(nt::host::decode_counter_switch(record,2)->view());
            if (attributed.snapshot().status!=CounterStatus::ready ||
                attributed.snapshot().totals!=std::vector<std::uint64_t>{80,3} || decoded->counters[0]!=100)
                throw std::runtime_error("ETW record data was not owned or attributed correctly");
            const auto reject_record=[&]() {
                try { nt::host::decode_counter_switch(record,2); }
                catch (const std::invalid_argument&) { return; }
                throw std::runtime_error("Incomplete ETW counter record accepted");
            };
            record.EventHeader.EventDescriptor.Version=3; reject_record();
            record.EventHeader.EventDescriptor.Version=6; reject_record();
            record.EventHeader.EventDescriptor.Version=static_cast<UCHAR>(version);
            for (const unsigned invalid_size : {0u,8u,23u,25u,27u,29u,32u,
                                                version==2 ? 28u : 24u}) {
                record.UserDataLength=static_cast<USHORT>(invalid_size); reject_record();
            }
            record.UserDataLength=payload_size;
            record.ExtendedDataCount=0; reject_record(); record.ExtendedDataCount=1;
            extensions[0].DataSize=8; reject_record(); extensions[0].DataSize=sizeof(readings);
            extensions[1]=extensions[0]; record.ExtendedDataCount=2; reject_record(); record.ExtendedDataCount=1;
            record.EventHeader.EventDescriptor.Opcode=50;
            if (nt::host::decode_counter_switch(record,2)) throw std::runtime_error("Unrelated event interpreted as counters");
        }
        // Original synthetic Windows-format records; no hardware event IDs assumed.
        std::vector<std::byte> bytes(64);
        const auto word=[&](unsigned at,std::uint32_t value) { std::memcpy(bytes.data()+at,&value,4); };
        const auto name=[&](unsigned at,const wchar_t* value) { std::memcpy(bytes.data()+at,value,4); };
        word(0,32); word(4,123); word(8,10); word(12,100); name(24,L"A");
        word(36,456); word(40,20); word(44,200); name(56,L"B");
        const auto sources=nt::host::parse_profile_sources(bytes);
        if (sources.size()!=2 || sources[0].id!=123 || sources[1].description!=L"B" ||
            sources[1].minimum_interval!=20 || sources[0].maximum_interval!=100)
            throw std::runtime_error("Profile source list decoding differs");
        const auto rejected=[&](std::span<const std::byte> input) {
            try { nt::host::parse_profile_sources(input); }
            catch (const std::invalid_argument&) { return; }
            throw std::runtime_error("Malformed profile source list accepted");
        };
        rejected(std::span(bytes).first(25));
        word(0,24); rejected(bytes);
        word(0,65); rejected(bytes);
        word(0,32); word(32,32); rejected(bytes);
        word(32,0); std::fill(bytes.begin()+56,bytes.end(),std::byte{1}); rejected(bytes);
        if (!nt::host::parse_profile_sources({}).empty()) throw std::runtime_error("Empty source list differs");
        std::cout<<"Native metric source and interval milestone passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
