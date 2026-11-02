#pragma once
#include <nt/ee_codegen.hpp>
#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fixture {
inline void verify_incremental_ee_modules() {
    std::vector<nt::r5900::Instruction> original;
    for (unsigned n=0;n<nt::ee_codegen::function_instruction_limit;++n)
        original.push_back(nt::r5900::decode(0x1000+n*4,0));
    const std::uint32_t helper[]{0xac820000,0x03e00008,0x24420001};
    for (unsigned n=0;n<std::size(helper);++n)
        original.push_back(nt::r5900::decode(0x40000+n*4,helper[n]));
    const auto emit=[](const auto& instructions) {
        std::map<std::size_t,std::ostringstream> streams;
        std::ostringstream metadata;
        nt::ee_codegen::emit_llvm(metadata,instructions,{}, {}, {}, {},[&](std::size_t id)->std::ostream& {
            if (streams.contains(id)) throw std::runtime_error("Duplicate native code unit identity");
            return streams[id];
        });
        std::map<std::size_t,std::string> result;
        for (const auto& [id,stream]:streams) result.emplace(id,stream.str());
        return result;
    };
    const auto before=emit(original);
    auto discovered=original;
    // Add a function below the existing far-away helper. This also changes the
    // complete code-protection ranges and the number of preceding native parts.
    discovered.push_back(nt::r5900::decode(0x2000,0x03e00008));
    discovered.push_back(nt::r5900::decode(0x2004,0x24420002));
    std::sort(discovered.begin(),discovered.end(),[](const auto& a,const auto& b) { return a.pc<b.pc; });
    const auto after=emit(discovered);
    if (!before.contains(0) || !before.contains(4) || before.size()!=2 || after.size()!=2 ||
        before.at(0)==after.at(0) || before.at(4)!=after.at(4) || after!=emit(discovered))
        throw std::runtime_error("Incremental discovery changed an unrelated native code unit");
}
}
