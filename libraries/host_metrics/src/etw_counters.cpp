#include <nt/etw_counters.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <evntcons.h>
#include <cstring>
#include <stdexcept>

namespace nt::host {
std::optional<OwnedSwitchSample> decode_counter_switch(const EVENT_RECORD& record,unsigned counter_count) {
    constexpr GUID thread_events{0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
    if (!counter_count || counter_count>16) throw std::invalid_argument("Invalid ETW counter count");
    if (!IsEqualGUID(record.EventHeader.ProviderId,thread_events) || record.EventHeader.EventDescriptor.Opcode!=36)
        return {};
    unsigned payload_size;
    switch (record.EventHeader.EventDescriptor.Version) {
    case 2: payload_size=24; break;
    case 5: payload_size=28; break;
    default: throw std::invalid_argument("Unsupported ETW context-switch event version");
    }
    // Both layouts retain the thread-ID prefix. Additional fields are not used
    // for counter attribution; do not infer support for other record versions.
    if (!record.UserData || record.UserDataLength!=payload_size || record.EventHeader.TimeStamp.QuadPart<0)
        throw std::invalid_argument("Malformed ETW context-switch payload");
    if (!record.ExtendedData || !record.ExtendedDataCount || record.ExtendedDataCount>64)
        throw std::invalid_argument("Missing or oversized ETW extension list");
    const EVENT_HEADER_EXTENDED_DATA_ITEM* counter_data=nullptr;
    for (unsigned index=0;index<record.ExtendedDataCount;++index) {
        const auto& item=record.ExtendedData[index];
        if (item.ExtType!=EVENT_HEADER_EXT_TYPE_PMC_COUNTERS) continue;
        if (counter_data) throw std::invalid_argument("Duplicate ETW counter extension");
        counter_data=&item;
    }
    if (!counter_data || !counter_data->DataPtr || counter_data->DataSize!=counter_count*sizeof(std::uint64_t))
        throw std::invalid_argument("ETW counter data does not match configured sources");
    std::uint32_t incoming,outgoing;
    std::memcpy(&incoming,record.UserData,4);
    std::memcpy(&outgoing,static_cast<const std::byte*>(record.UserData)+4,4);
    OwnedSwitchSample sample{GetEventProcessorIndex(&record),outgoing,incoming,
        static_cast<std::uint64_t>(record.EventHeader.TimeStamp.QuadPart),std::vector<std::uint64_t>(counter_count)};
    std::memcpy(sample.counters.data(),reinterpret_cast<const void*>(static_cast<std::uintptr_t>(counter_data->DataPtr)),counter_data->DataSize);
    return sample;
}
}
