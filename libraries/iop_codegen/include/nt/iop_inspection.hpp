#pragma once
#include <nt/iop_ir.hpp>
#include <array>

namespace nt::iop_codegen::ir {
// Offline candidate inspection only. It never executes instructions or changes
// the production AOT coverage rules.
Instruction decode_instruction(std::uint32_t pc,std::uint32_t word);
struct CallPattern {
    std::uint32_t address=0,target=0;
    bool indirect=false;
    std::optional<elf::IopImport> import;
    std::array<std::optional<std::set<std::uint32_t>>,8> arguments;
    std::map<std::int32_t,std::set<std::uint32_t>> stack_words;
    std::map<unsigned,std::array<std::optional<std::uint8_t>,16>> stack_arguments;
    std::set<std::uint32_t> code_candidates;
};
struct Neighborhood {
    std::uint32_t anchor=0,first=0,last=0;
    std::vector<CallPattern> calls;
    std::set<std::uint32_t> function_candidates;
    std::map<std::string,std::size_t> patterns;
    std::map<std::uint32_t,std::string> unknown;
};
Neighborhood inspect_neighborhood(const elf::IopModule& module,std::uint32_t anchor,std::uint32_t radius);
}
