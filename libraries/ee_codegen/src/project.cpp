#include <nt/ee_codegen.hpp>
#include <nt/staged_directory.hpp>
#include "project_internal.hpp"
#include "native_layout.hpp"
#include <stdexcept>

namespace nt::ee_codegen {
void project(const std::filesystem::path& input,const std::filesystem::path& destination,std::span<const PayloadMapping> mappings,
             std::span<const std::filesystem::path> images,std::span<const std::filesystem::path> observations,bool function_symbols,bool whole_code) {
    const auto prepared=project_detail::prepare_project(input,mappings,images,false,observations,function_symbols,whole_code);
    if (!prepared.discovered.unresolved.empty())
        throw std::runtime_error("EE discovery reached unmapped code at "+hex32(*prepared.discovered.unresolved.begin()));
    StagedDirectory staged(destination);
    project_detail::emit_project(staged.path(),prepared);
    const auto layout=prepare_native_layout(prepared.layouts);
    project_detail::write_project_reports(staged.path(),prepared,mappings,layout);
    staged.publish();
}
FlowVerification verify_flow(const std::filesystem::path& input,const std::filesystem::path& destination,
                             std::span<const PayloadMapping> mappings,std::span<const std::filesystem::path> images,
                             std::span<const std::filesystem::path> observations,bool function_symbols,bool whole_code) {
    const auto prepared=project_detail::prepare_project(input,mappings,images,true,observations,function_symbols,whole_code);
    const auto diagnostics=project_detail::diagnose_flow(prepared);
    StagedDirectory staged(destination);
    project_detail::write_flow_reports(staged.path(),prepared,diagnostics);
    staged.publish();
    return diagnostics.result;
}
}
