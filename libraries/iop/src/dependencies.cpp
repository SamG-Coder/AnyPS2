#include <nt/iop_dependencies.hpp>
#include <nt/reboot_image.hpp>
#include <set>
#include <algorithm>

namespace nt::iop {
DependencyInventory inspect_dependencies(ByteView image) {
    DependencyInventory result;
    std::set<std::string,std::less<>> providers;
    for (const auto& file : disc::parse_reboot_image(image)) {
        const auto bytes=image.subspan(file.offset,file.size);
        if (bytes.size()<4 || le32(bytes,0)!=0x464c457f) continue;
        // Relocate private copies to a nonzero scratch base so ordinal zero
        // can refer to the first instruction without resembling a terminator.
        const auto module=elf::load_iop_module(bytes,0x10000);
        ModuleDependencies entry{file.name,static_cast<std::uint32_t>(file.offset),elf::iop_imports(module),{}};
        for (std::uint32_t offset=0;offset+4<=module.initialized_size;offset+=4)
            if (le32(module.memory,offset)==0x41c00000) {
                auto table=read_exports(module,module.base+offset);
                providers.insert(table.name);
                entry.exports.push_back(std::move(table));
            }
        result.modules.push_back(std::move(entry));
    }
    for (const auto& consumer : result.modules) {
        ModuleImports missing{consumer.module,consumer.imports};
        std::erase_if(missing.imports,[&](const auto& import) { return providers.contains(import.library); });
        if (!missing.imports.empty()) result.missing_libraries.push_back(std::move(missing));
    }
    return result;
}
StartupPlan plan_startup(std::span<const ModuleDependencies> modules,std::vector<ExportTable> available) {
    StartupPlan result;
    std::vector<bool> scheduled(modules.size(),false);
    const auto resolved=[&](const elf::IopImport& import) {
        const ExportTable* selected=nullptr;
        for (const auto& table : available) {
            if (table.name!=import.library || (table.version>>8)!=(import.version>>8) || table.version<import.version) continue;
            if (!selected || table.version>selected->version) selected=&table;
        }
        return selected && import.ordinal<selected->functions.size();
    };
    for (;;) {
        std::optional<std::size_t> ready;
        for (std::size_t i=0;i<modules.size();++i)
            if (!scheduled[i] && std::all_of(modules[i].imports.begin(),modules[i].imports.end(),resolved)) { ready=i; break; }
        if (!ready) break;
        const auto& module=modules[*ready];
        scheduled[*ready]=true;
        result.file_offsets.push_back(module.file_offset);
        available.insert(available.end(),module.exports.begin(),module.exports.end());
    }
    for (std::size_t i=0;i<modules.size();++i) {
        if (scheduled[i]) continue;
        ModuleImports blocked{modules[i].module,modules[i].imports};
        std::erase_if(blocked.imports,resolved);
        result.blocked.push_back(std::move(blocked));
    }
    return result;
}
}
