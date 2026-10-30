#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nt::host {
struct ProfileSource {
    std::uint32_t id,minimum_interval,maximum_interval;
    std::wstring description;
};
// Decode the Windows profile-source response without retaining native pointers.
std::vector<ProfileSource> parse_profile_sources(std::span<const std::byte> bytes);
// Read-only capability query. Enumeration does not enable counters or establish
// measurement availability, per-thread attribution, or PS2 metric equivalence.
std::vector<ProfileSource> profile_sources();
}
