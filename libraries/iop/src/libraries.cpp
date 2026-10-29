#include <nt/iop_libraries.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
ExportTable read_exports(const elf::IopModule& owner, std::uint32_t address) {
    if (owner.initialized_size>owner.memory.size() || owner.base%4 ||
        std::uint64_t(owner.base)+owner.memory.size()>2*1024*1024 || address%4 || address<owner.base)
        throw std::runtime_error("IOP exports: invalid owner or table address");
    const auto bytes=ByteView(owner.memory).first(owner.initialized_size);
    const std::size_t table=address-owner.base;
    require_range(bytes,table,24);
    if (le32(bytes,table)!=0x41c00000 || le32(bytes,table+4) || le16(bytes,table+10))
        throw std::runtime_error("IOP exports: unsupported table header or mode");
    const auto field=bytes.subspan(table+12,8);
    const auto end=std::find(field.begin(),field.end(),0);
    if (end==field.begin() || std::any_of(field.begin(),end,[](auto c) { return c<33 || c>126; }))
        throw std::runtime_error("IOP exports: invalid library name");
    const std::string name(field.begin(),end);
    ExportTable library{name,le16(bytes,table+8),{}};
    for (std::size_t at=table+20;;at+=4) {
        const auto target=le32(bytes,at);
        if (!target) break;
        if (library.functions.size()==65536) throw std::runtime_error("IOP exports: ordinal limit exceeded");
        if (target%4 || target<owner.base || std::uint64_t(target)+4>std::uint64_t(owner.base)+bytes.size())
            throw std::runtime_error("IOP exports: function is outside owner executable image");
        library.functions.push_back(target);
    }
    return library;
}
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
