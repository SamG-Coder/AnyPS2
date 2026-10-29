#pragma once
#include <nt/iop_native.hpp>
#include <algorithm>
#include <array>

namespace nt::iop {
// Input extents are validated by the service before encoding. Legacy directory
// wrappers copy stat and name only; the caller's privdata word is not replaced.
inline std::array<std::uint8_t,296> legacy_directory_record(std::span<const std::uint8_t> extended) {
    std::array<std::uint8_t,296> result{};
    std::copy_n(extended.begin(),40,result.begin());
    const auto mode=load_native_word(extended,0);
    const auto flags=((mode&0x4000) ? 8U : 0U)|((mode&0x2000) ? 16U : 0U)|((mode&0x1000) ? 32U : 0U)|
        ((mode&0x124) ? 4U : 0U)|((mode&0x92) ? 2U : 0U)|((mode&0x49) ? 1U : 0U);
    store_native_word(result,0,flags);
    const auto name=extended.subspan(64,256);
    const auto end=std::find(name.begin(),name.end(),0);
    std::copy(name.begin(),end,result.begin()+40);
    return result;
}
}
