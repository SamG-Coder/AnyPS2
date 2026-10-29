#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace nt::iop {
// One descriptor namespace shared by native mounts and compiled drivers.
// Console descriptors 0..2 are reserved for explicit console providers.
class NativeFileDescriptors {
public:
    struct Entry { const void* owner; std::uint32_t local; };
    std::optional<Entry> find(std::uint32_t fd) const {
        return fd>=3 && fd-3<entries_.size() ? entries_[fd-3] : std::optional<Entry>{};
    }
    std::optional<std::uint32_t> allocate(const void* owner,std::uint32_t local) {
        for (std::uint32_t index=0;index<entries_.size();++index) if (!entries_[index]) {
            entries_[index]=Entry{owner,local}; return index+3;
        }
        return {};
    }
    void release(std::uint32_t fd,const void* owner) {
        const auto entry=find(fd);
        if (entry && entry->owner==owner) entries_[fd-3].reset();
    }
private:
    std::array<std::optional<Entry>,64> entries_{};
};
}
