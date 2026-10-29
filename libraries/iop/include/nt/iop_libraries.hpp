#pragma once
#include <nt/iop_module.hpp>
#include <map>

namespace nt::iop {
using ExportTable=elf::IopExports;
inline ExportTable read_exports(const elf::IopModule& owner,std::uint32_t table_address) {
    return elf::read_iop_exports(owner,table_address);
}
// Only explicitly registered export tables are visible to import binding.
// This registry does not start modules or auto-register tables found in an image.
class Libraries {
public:
    void register_exports(const elf::IopModule& owner, std::uint32_t table_address);
    std::optional<std::uint32_t> resolve(const elf::IopImport& import) const;
    void bind(elf::IopModule& module) const;
    void reset() { libraries_.clear(); }
    std::size_t size() const { return libraries_.size(); }
private:
    struct Library {
        std::uint16_t version;
        std::vector<std::uint32_t> functions;
    };
    std::map<std::string,Library,std::less<>> libraries_;
};
}
