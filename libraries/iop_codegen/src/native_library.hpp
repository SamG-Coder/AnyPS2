#pragma once
#include <nt/iop_ir.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace nt::iop_codegen::ir {
// Replace a public library ABI, never an observed game instruction address.
// Entries are still emitted as LLVM functions, which call native services.
inline std::optional<Program> native_library(const elf::IopModule& module,std::span<const std::uint32_t> entries) {
    const auto exports=elf::iop_exports(module);
    const auto found=std::find_if(exports.begin(),exports.end(),[](const auto& library) { return library.name=="libsd"; });
    if (found==exports.end()) return {};
    if (exports.size()!=1 || (found->version>>8)!=1 || found->functions.size()<28 || found->functions.size()>34 ||
        found->functions.front()!=module.entry)
        throw std::runtime_error("Native sound replacement requires a LIBSD 1.x public export ABI");
    Program program; program.entry=module.entry;
    for (unsigned ordinal=0;ordinal<found->functions.size();++ordinal) {
        const auto address=found->functions[ordinal];
        const auto extent=module.info ? module.info->text_size : module.initialized_size;
        if (address%4 || address<module.base || address-module.base>extent || extent-(address-module.base)<8)
            throw std::runtime_error("Native sound export is outside the declared executable extent");
        Instruction entry; entry.address=address; entry.operation=Operation::native_import;
        entry.flow=Flow::native_return; entry.immediate=0x150000U|(ordinal==3 ? 1U : ordinal);
        const auto [previous,inserted]=program.instructions.emplace(address,entry);
        if (!inserted && previous->second.immediate!=entry.immediate)
            throw std::runtime_error("Native sound export aliases have conflicting ABI operations");
        program.protected_words.insert(address); program.protected_words.insert(address+4);
    }
    for (const auto entry:entries) if (!program.instructions.contains(entry))
        throw std::runtime_error("Native sound replacement cannot expose private console implementation code");
    return program;
}
}
