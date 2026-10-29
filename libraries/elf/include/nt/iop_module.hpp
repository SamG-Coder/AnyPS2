#pragma once
#include <nt/binary.hpp>
#include <functional>
#include <optional>

namespace nt::elf {
struct IopModuleInfo {
    std::string name;
    std::uint16_t version;
    std::uint32_t global_pointer, text_size, data_size, bss_size;
};
struct IopModule {
    std::uint32_t base, entry;
    Bytes memory;
    std::uint32_t initialized_size = 0;
    std::optional<IopModuleInfo> info;
};
// Prepare a relocatable IRX in private storage. The caller owns allocation,
// import binding and execution; errors never partially modify guest RAM.
IopModule load_iop_module(ByteView file, std::uint32_t base);
struct IopExports {
    std::string name;
    std::uint16_t version;
    std::vector<std::uint32_t> functions;
    std::uint32_t address=0;
};
IopExports read_iop_exports(const IopModule& owner,std::uint32_t table_address);
// Conservative image inventory only. Finding a table does not register a library.
std::vector<IopExports> iop_exports(const IopModule& module);
struct IopImport {
    std::string library;
    std::uint16_t version, ordinal;
    std::uint32_t offset;
    std::uint32_t table_offset=0;
};
std::vector<IopImport> iop_imports(const IopModule& module);
// Resolution policy belongs to the runtime's registered libraries. A resolver
// must return a genuine guest entry point, or no value for an unsupported import.
using IopImportResolver = std::function<std::optional<std::uint32_t>(const IopImport&)>;
void bind_iop_imports(IopModule& module, const IopImportResolver& resolve);
}
