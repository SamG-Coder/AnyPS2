#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace nt::ee {
// Owned by the cooperative native runner. No host blocking or guest scheduler
// is hidden here: a contended wait remains pending at its syscall boundary.
class Semaphores {
public:
    struct Record { std::uint32_t count,maximum,attributes,option; };
    static constexpr std::uint32_t capacity=256;
    const Record* find(std::uint32_t id) const {
        return id && id<=capacity && records_[id-1] ? &*records_[id-1] : nullptr;
    }
    std::uint32_t create(Record record) {
        if (!record.maximum || record.maximum>0x7fffffffU || record.count>record.maximum) return 0;
        for (std::uint32_t i=0;i<capacity;++i) if (!records_[i]) {
            records_[i]=record; return i+1;
        }
        return 0;
    }
    bool erase(std::uint32_t id) {
        if (!find(id)) return false;
        records_[id-1].reset(); return true;
    }
    bool signal(std::uint32_t id) {
        const auto value=find(id);
        if (!value || value->count==value->maximum) return false;
        ++records_[id-1]->count; return true;
    }
    bool acquire(std::uint32_t id) {
        const auto value=find(id);
        if (!value || !value->count) return false;
        --records_[id-1]->count; return true;
    }
private:
    std::array<std::optional<Record>,capacity> records_{};
};
}
