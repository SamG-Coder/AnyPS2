#pragma once
#include <nt/ee_codegen.hpp>
#include <nt/analysis.hpp>
#include "flow_observation.hpp"

namespace nt::ee_codegen {
struct NativeLayout;
namespace project_detail {
struct PreparedProject {
    struct Pass { unsigned image=1; std::size_t index,instructions,new_roots,new_registrations; };
    bool whole_code=false;
    elf::Executable executable;
    analysis::Program discovered;
    std::vector<CodeRange> ranges;
    std::vector<CodeRange> selected_regions;
    std::vector<CodePayload> payloads;
    Bytes image;
    std::vector<LayoutDeclaration> layouts;
    std::vector<ExecutableImage> images;
    std::vector<analysis::CallSite> calls;
    std::vector<analysis::PointerReference> pointers;
    std::vector<analysis::CallbackFlowGap> flow_gaps;
    std::vector<Pass> passes;
    std::vector<observation::Edge> observations;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> function_roots;
    std::uint64_t input_identity=0;
    std::set<std::uint32_t> registered_calls;
    std::size_t wrapper_candidates=0;
    std::uint32_t syscall_table=0;
};

struct FlowDiagnostics {
    FlowVerification result;
    EeRejection rejection;
    std::string layout_error;
};

PreparedProject prepare_project(const std::filesystem::path& input,std::span<const PayloadMapping> mappings,
    std::span<const std::filesystem::path> images,bool diagnose_flow,
    std::span<const std::filesystem::path> observations,bool function_symbols,bool whole_code);
FlowDiagnostics diagnose_flow(const PreparedProject& prepared);
void emit_project(const std::filesystem::path& output,const PreparedProject& prepared);
void write_project_reports(const std::filesystem::path& output,const PreparedProject& prepared,
    std::span<const PayloadMapping> mappings,const NativeLayout& layout);
void write_flow_reports(const std::filesystem::path& output,const PreparedProject& prepared,
    const FlowDiagnostics& diagnostics);
}
}
