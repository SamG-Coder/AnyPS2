#include <nt/iop_libraries.hpp>
#include <stdexcept>

namespace nt::iop {
void Libraries::register_exports(const elf::IopModule& owner, std::uint32_t address) {
    auto table=read_exports(owner,address);
    if (libraries_.contains(table.name)) throw std::runtime_error("IOP exports: library already registered: "+table.name);
    libraries_.emplace(std::move(table.name),Library{table.version,std::move(table.functions)});
}
std::optional<std::uint32_t> Libraries::resolve(const elf::IopImport& import) const {
    const auto found=libraries_.find(import.library);
    if (found==libraries_.end()) return {};
    const auto& library=found->second;
    // Initially require an exact ABI version. Compatible-minor replacement and
    // duplicate providers need explicit lifecycle rules before being supported.
    if (import.version!=library.version || import.ordinal>=library.functions.size()) return {};
    return library.functions[import.ordinal];
}
void Libraries::bind(elf::IopModule& module) const {
    elf::bind_iop_imports(module,[this](const auto& import) { return resolve(import); });
}
}
