#include <nt/iop_module.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::elf {
std::vector<IopImport> iop_imports(const IopModule& module) {
    if (module.initialized_size>module.memory.size()) throw std::runtime_error("IOP imports: invalid image extent");
    const auto bytes=ByteView(module.memory).first(module.initialized_size);
    std::vector<IopImport> imports;
    for (std::size_t at=0;at+4<=bytes.size();at+=4) {
        if (le32(bytes,at)!=0x41e00000) continue;
        require_range(bytes,at,20);
        if (le32(bytes,at+4) || le16(bytes,at+10))
            throw std::runtime_error("IOP imports: linked tables or nonzero modes are unsupported");
        const auto field=bytes.subspan(at+12,8);
        const auto end=std::find(field.begin(),field.end(),0);
        if (end==field.begin() || std::any_of(field.begin(),end,[](auto c) { return c<33 || c>126; }))
            throw std::runtime_error("IOP imports: invalid library name");
        const std::string library(field.begin(),end);
        const auto version=le16(bytes,at+8);
        at+=20;
        for (;;) {
            require_range(bytes,at,8);
            const auto jump=le32(bytes,at), delay=le32(bytes,at+4);
            if (!jump && !delay) break;
            if (jump!=0x03e00008 || (delay&0xffff0000)!=0x24000000)
                throw std::runtime_error("IOP imports: invalid unbound import stub");
            imports.push_back({library,version,static_cast<std::uint16_t>(delay),static_cast<std::uint32_t>(at)});
            at+=8;
        }
        at+=4; // The enclosing loop advances past the second terminator word.
    }
    return imports;
}
void bind_iop_imports(IopModule& module, const IopImportResolver& resolve) {
    const auto imports=iop_imports(module);
    std::vector<std::uint32_t> targets;
    targets.reserve(imports.size());
    for (const auto& import : imports) {
        const auto target=resolve(import);
        if (!target) throw std::runtime_error("IOP imports: unresolved "+import.library+" ordinal "+std::to_string(import.ordinal));
        const std::uint64_t pc=std::uint64_t(module.base)+import.offset+4;
        if (*target%4 || !*target || *target>=2*1024*1024 || pc>=2*1024*1024)
            throw std::runtime_error("IOP imports: entry point exceeds executable IOP RAM");
        targets.push_back(0x08000000U|(*target>>2));
    }
    // Commit only after every import has resolved and every target is valid.
    for (std::size_t i=0;i<imports.size();++i)
        for (unsigned byte=0;byte<4;++byte)
            module.memory[imports[i].offset+byte]=static_cast<std::uint8_t>(targets[i]>>(8*byte));
}
}
