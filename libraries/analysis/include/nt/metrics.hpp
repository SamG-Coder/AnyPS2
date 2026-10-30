#pragma once
#include <nt/analysis.hpp>
#include <optional>
#include <vector>

namespace nt::analysis {
enum class MetricUseKind { stored_value, memory_address, system_write, flow_boundary, unknown_operation, missing_code, budget, unresolved_memory };
struct MetricUse {
    std::uint32_t pc;
    MetricUseKind kind;
    std::optional<std::uint32_t> address;
};
struct MetricMemoryUse {
    std::uint32_t pc,address;
    unsigned bytes;
    bool write;
    std::uint32_t base=0;
};
enum class MetricPointerKind { call_argument, return_value, stored_pointer };
struct MetricPointerUse {
    std::uint32_t pc,value;
    unsigned reg;
    MetricPointerKind kind;
    std::optional<std::uint32_t> target;
    std::vector<MetricMemoryUse> memory_uses;
    std::vector<MetricUse> boundaries;
    std::optional<std::uint32_t> storage;
};
struct MetricSite {
    std::uint32_t pc;
    bool write, counter;
    unsigned index, reg;
    std::vector<MetricUse> uses;
    std::vector<MetricMemoryUse> memory_uses;
    std::vector<MetricPointerUse> pointer_uses;
    bool pointer_search_incomplete=false;
};
// Executable-segment candidates, not proof of reachability. Follow register
// dependencies only until a flow/unsupported boundary; never authorize removal.
std::vector<MetricSite> inspect_metrics(ByteView bytes,const elf::Executable& executable,
    std::size_t window=128);
void describe_metrics(std::ostream& out,std::span<const MetricSite> sites);
}
