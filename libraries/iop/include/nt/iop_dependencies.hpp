#pragma once
#include <nt/iop_libraries.hpp>

namespace nt::iop {
struct ModuleImports {
    std::string module;
    std::vector<elf::IopImport> imports;
};
struct ModuleDependencies {
    std::string module;
    std::uint32_t file_offset;
    std::vector<elf::IopImport> imports;
    std::vector<ExportTable> exports;
};
// Static archive inventory only. Finding an export does not register a live
// service, guarantee version compatibility, or establish an initialization order.
struct DependencyInventory {
    std::vector<ModuleDependencies> modules;
    std::vector<ModuleImports> missing_libraries;
};
DependencyInventory inspect_dependencies(ByteView reboot_image);
struct StartupPlan {
    std::vector<std::uint32_t> file_offsets;
    std::vector<ModuleImports> blocked;
};
// Preserve input order among ready modules. Conservatively require matching
// major, sufficient minor and ordinal; static exports do not prove registration.
StartupPlan plan_startup(std::span<const ModuleDependencies> modules,std::vector<ExportTable> available);
}
