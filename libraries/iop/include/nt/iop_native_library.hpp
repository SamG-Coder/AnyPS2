#pragma once
#include <nt/iop_libraries.hpp>

namespace nt::iop {
// Shared export-table/trampoline storage for original C++ service providers.
class NativeLibrary {
public:
    NativeLibrary(std::string_view name,std::uint16_t version,unsigned count,std::uint32_t base);
    static constexpr unsigned allocation_size(unsigned count) { return ((24+count*4+15)&~15U)+((count*12+31)&~15U); }
    void install(std::span<std::uint8_t> ram,Libraries& libraries);
    std::optional<unsigned> ordinal(std::uint32_t pc,std::size_t ram_size) const;
    const elf::IopModule& module() const { return module_; }
    std::uint32_t registration_flag() const { return module_.base+code_offset_+count_*12; }
private:
    elf::IopModule module_;
    unsigned count_, code_offset_;
    bool installed_=false;
};
}
