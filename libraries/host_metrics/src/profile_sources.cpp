#include <nt/host_metrics.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <evntrace.h>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace nt::host {
std::vector<ProfileSource> parse_profile_sources(std::span<const std::byte> bytes) {
    constexpr auto header=offsetof(PROFILE_SOURCE_INFO,Description);
    std::vector<ProfileSource> result;
    std::size_t offset=0;
    while (offset<bytes.size()) {
        if (bytes.size()-offset<header+sizeof(wchar_t))
            throw std::invalid_argument("Truncated profile source record");
        PROFILE_SOURCE_INFO native{};
        std::memcpy(&native,bytes.data()+offset,header);
        const auto size=native.NextEntryOffset ? std::size_t(native.NextEntryOffset) : bytes.size()-offset;
        if (size<header+sizeof(wchar_t) || size>bytes.size()-offset || size%sizeof(wchar_t))
            throw std::invalid_argument("Invalid profile source extent");
        ProfileSource source{native.Source,native.MinInterval,native.MaxInterval,{}};
        bool terminated=false;
        for (auto cursor=header;cursor+sizeof(wchar_t)<=size;cursor+=sizeof(wchar_t)) {
            wchar_t character;
            std::memcpy(&character,bytes.data()+offset+cursor,sizeof(character));
            if (!character) { terminated=true; break; }
            source.description.push_back(character);
        }
        if (!terminated) throw std::invalid_argument("Unterminated profile source description");
        result.push_back(std::move(source));
        if (!native.NextEntryOffset) break;
        offset+=size;
        if (offset==bytes.size()) throw std::invalid_argument("Missing final profile source record");
    }
    return result;
}
std::vector<ProfileSource> profile_sources() {
    ULONG required=0;
    auto status=TraceQueryInformation(0,TraceProfileSourceListInfo,nullptr,0,&required);
    constexpr ULONG maximum_bytes=1024*1024;
    for (unsigned attempt=0;attempt<4;++attempt) {
        if (status!=ERROR_SUCCESS && status!=ERROR_BAD_LENGTH && status!=ERROR_INSUFFICIENT_BUFFER)
            throw std::system_error(status,std::system_category(),"Query Windows profile sources");
        if (!required) {
            if (status==ERROR_SUCCESS) return {};
            throw std::runtime_error("Windows profile source query returned no required length");
        }
        if (required>maximum_bytes) throw std::runtime_error("Windows profile source response exceeds 1 MiB");
        std::vector<std::byte> bytes(required);
        ULONG returned=0;
        status=TraceQueryInformation(0,TraceProfileSourceListInfo,bytes.data(),required,&returned);
        if (status==ERROR_SUCCESS) {
            if (returned>bytes.size()) throw std::runtime_error("Windows profile source length exceeds buffer");
            return parse_profile_sources(std::span(bytes).first(returned));
        }
        required=returned;
    }
    throw std::system_error(status,std::system_category(),"Windows profile source query did not stabilize");
}
}
