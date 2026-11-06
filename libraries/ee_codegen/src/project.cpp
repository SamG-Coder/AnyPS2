#include <nt/ee_codegen.hpp>
#include <nt/staged_directory.hpp>
#include "project_internal.hpp"
#include "native_layout.hpp"
#include <stdexcept>

namespace nt::ee_codegen {
void project(const std::filesystem::path& input,const std::filesystem::path& destination,std::span<const PayloadMapping> mappings,
             std::span<const std::filesystem::path> images,std::span<const std::filesystem::path> observations,bool function_symbols,bool whole_code,
             std::span<const InstructionPatch> patches) {
    const auto prepared=project_detail::prepare_project(input,mappings,images,false,observations,function_symbols,whole_code,patches);
    // An unmapped target inside a loaded image is a discovery gap and stops
    // conversion. A target outside every loaded image (for example console
    // kernel memory a game tries to call or patch) has no code to convert: it
    // compiles to the runtime unmapped-code stop and is listed in gaps.tsv.
    const auto loaded=[&](std::uint32_t address) {
        for (const auto& segment:prepared.executable.segments)
            if (segment.memory_size && address-segment.address<segment.memory_size) return true;
        for (const auto& image:prepared.images) for (const auto& payload:image.segments)
            if (address-payload.address<payload.bytes.size()) return true;
        return false;
    };
    for (const auto address:prepared.discovered.unresolved)
        if (loaded(address)) throw std::runtime_error("EE discovery reached unmapped code at "+hex32(address));
    StagedDirectory staged(destination);
    project_detail::emit_project(staged.path(),prepared);
    const auto layout=prepare_native_layout(prepared.layouts);
    project_detail::write_project_reports(staged.path(),prepared,mappings,layout);
    staged.publish();
}
FlowVerification verify_flow(const std::filesystem::path& input,const std::filesystem::path& destination,
                             std::span<const PayloadMapping> mappings,std::span<const std::filesystem::path> images,
                             std::span<const std::filesystem::path> observations,bool function_symbols,bool whole_code,
                             std::span<const InstructionPatch> patches) {
    const auto prepared=project_detail::prepare_project(input,mappings,images,true,observations,function_symbols,whole_code,patches);
    const auto diagnostics=project_detail::diagnose_flow(prepared);
    StagedDirectory staged(destination);
    project_detail::write_flow_reports(staged.path(),prepared,diagnostics);
    staged.publish();
    return diagnostics.result;
}
}
