#pragma once
#include <nt/binary.hpp>

namespace nt::iop {
// Original position-independent IRX entry which calls loadcore ordinal 6.
// The completion word is set only after the guest registration call returns 0.
Bytes registration_module(std::uint32_t exports,std::uint32_t completion,std::uint16_t loadcore_version);
}
