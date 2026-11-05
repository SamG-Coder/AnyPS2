#pragma once
#include <nt/iop_ir.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace nt::iop_codegen::ir {
inline bool native_library_module(const elf::IopModule& module) {
    const auto exports=elf::iop_exports(module);
    return std::any_of(exports.begin(),exports.end(),[](const auto& library) {
        return library.name=="libsd" || library.name=="usbd" || library.name=="dev9";
    });
}
// Replace a public library ABI, never an observed game instruction address.
// Entries are still emitted as LLVM functions, which call native services.
inline std::optional<Program> native_library(const elf::IopModule& module,std::span<const std::uint32_t> entries) {
    const auto exports=elf::iop_exports(module);
    const auto found=std::find_if(exports.begin(),exports.end(),[](const auto& library) {
        return library.name=="libsd" || library.name=="usbd" || library.name=="dev9";
    });
    if (found==exports.end()) return {};
    const bool sound=found->name=="libsd";
    const bool expansion=found->name=="dev9";
    const unsigned minimum=sound ? 28U : expansion ? 10U : 14U;
    const unsigned maximum=sound ? 34U : expansion ? 16U : 19U;
    if (exports.size()!=1 || (found->version>>8)!=1 || found->functions.size()<minimum || found->functions.size()>maximum ||
        ((sound || expansion) && found->functions.front()!=module.entry))
        throw std::runtime_error("Native library replacement requires a supported 1.x public export ABI");
    Program program; program.entry=module.entry;
    const auto append=[&](std::uint32_t address,std::uint32_t selector) {
        const auto extent=module.info ? module.info->text_size : module.initialized_size;
        if (address%4 || address<module.base || address-module.base>extent || extent-(address-module.base)<8)
            throw std::runtime_error("Native library entry is outside the declared executable extent");
        Instruction entry; entry.address=address; entry.operation=Operation::native_import;
        entry.flow=Flow::native_return; entry.immediate=(sound ? 0x150000U : expansion ? 0x170000U : 0x160000U)|selector;
        const auto [previous,inserted]=program.instructions.emplace(address,entry);
        if (!inserted && previous->second.immediate!=entry.immediate)
            throw std::runtime_error("Native library export aliases have conflicting ABI operations");
        program.protected_words.insert(address); program.protected_words.insert(address+4);
    };
    for (unsigned ordinal=0;ordinal<found->functions.size();++ordinal) {
        auto selector=ordinal==3 || (!sound && ordinal==0) ? 1U : ordinal;
        if (!sound && ordinal==0 && found->functions[ordinal]==module.entry) selector=0x100;
        append(found->functions[ordinal],selector);
    }
    if (!sound) append(module.entry,0x100);
    for (const auto entry:entries) if (!program.instructions.contains(entry))
        throw std::runtime_error("Native library replacement cannot expose private console implementation code");
    return program;
}
}
