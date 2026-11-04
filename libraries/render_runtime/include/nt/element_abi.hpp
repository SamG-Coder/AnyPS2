#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace nt::render {
struct ElementEntry {
    std::array<std::uint16_t,16> integers{};  // vi0..vi15
    std::array<std::array<std::uint32_t,4>,32> vectors{};
    std::array<std::uint32_t,4> accumulator{};
    std::uint32_t immediate=0,quotient=0,p=0,top=0,itop=0;
};
inline constexpr unsigned element_parameter_words=156;
std::vector<std::uint32_t> element_parameters(const ElementEntry&,unsigned first_element,unsigned count);
}
