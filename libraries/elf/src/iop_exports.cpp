#include <nt/iop_module.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::elf {
IopExports read_iop_exports(const IopModule& owner, std::uint32_t address) {
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
    IopExports library{name,le16(bytes,table+8),{},address};
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
std::vector<IopExports> iop_exports(const IopModule& module) {
    if (module.initialized_size>module.memory.size())
        throw std::runtime_error("IOP exports: initialized size exceeds image");
    const auto bytes=ByteView(module.memory).first(module.initialized_size);
    std::vector<IopExports> tables;
    for (std::size_t offset=0;offset+4<=bytes.size();offset+=4) {
        if (le32(bytes,offset)==0x41c00000)
            tables.push_back(read_iop_exports(module,module.base+static_cast<std::uint32_t>(offset)));
    }
    return tables;
}
}
