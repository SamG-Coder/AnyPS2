#pragma once
#include <nt/ee_state.hpp>

namespace nt::ee {
enum class Stop : std::uint32_t { budget, syscall, unmapped_code, invalid_state, memory_fault, code_write, unsupported_numeric };
// RAM is caller-owned; the generated code never fetches instructions from it.
struct Memory {
    std::uint8_t* data=nullptr;
    std::uint64_t size=0;
    std::uint32_t fault_address=0;
};
// Syscall boundaries retain PC and do not retire until a native service handles them.
using Entry = Stop (*)(runtime::State*,Memory*,std::uint64_t);
}
