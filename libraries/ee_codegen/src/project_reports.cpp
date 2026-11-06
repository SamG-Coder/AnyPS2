#include "project_internal.hpp"
#include "native_layout.hpp"
#include <sstream>

namespace nt::ee_codegen::project_detail {
namespace {
void write_discovery_reports(const std::filesystem::path& output,const PreparedProject& prepared,bool verification=false) {
    std::ostringstream coverage;
    coverage<<"selection="<<(prepared.whole_code ? "whole-declared-EE-code" : "reachable-flow")<<"\n"
        <<"selected_instructions="<<prepared.discovered.instructions.size()<<"\n"
        <<"unverified_ranges="<<prepared.unverified_code.size()<<"\n"
        <<"runtime_target_selection=dynamic\n";
    write_text_file(output/"code-coverage.txt",coverage.str());
    std::ostringstream regions;
    regions<<"image\taddress\tbytes\tselected_words\n";
    for (const auto& range:prepared.selected_regions)
        regions<<range.image<<'\t'<<hex32(range.address)<<'\t'<<range.size<<'\t'<<range.size/4<<'\n';
    write_text_file(output/"code-regions.tsv",regions.str());
    std::ostringstream functions;
    functions<<"image\tentry\tevidence\n";
    for (const auto& [image,entry]:prepared.function_roots) functions<<image<<'\t'<<hex32(entry)<<"\tELF-STT_FUNC\n";
    write_text_file(output/"function-roots.tsv",functions.str());
    write_text_file(output/"input-identity.txt","format=anyps2-ee-input-v1\ninput_identity="+std::to_string(prepared.input_identity)+"\n");
    std::ostringstream observed;
    observed<<"image\tsite\ttarget\tevidence\tcoverage\n";
    for (const auto& edge:prepared.observations)
        observed<<edge.image<<'\t'<<hex32(edge.site)<<'\t'<<hex32(edge.target)
            <<"\tvalidated-native-stop\t"<<(verification ? "discovered-observed-target" : "generated-observed-target")<<'\n';
    write_text_file(output/"observed-call-edges.tsv",observed.str());
    std::ostringstream calls;
    calls<<"pc\tkind\ttarget\tcoverage\n";
    for (const auto& call:prepared.calls) {
        calls<<hex32(call.pc)<<'\t'<<analysis::name(call.kind)<<'\t';
        if (call.target) calls<<hex32(*call.target);
        calls<<'\t';
        if (call.target) {
            if (!prepared.discovered.instructions.contains(*call.target)) calls<<"missing";
            else if (verification) calls<<(call.initial_data_candidate ? "discovered-initial-candidate" : "discovered");
            else calls<<(call.initial_data_candidate ? "compiled-initial-candidate" : "compiled");
        }
        else if (call.kind==analysis::CallKind::service) calls<<"native-service-boundary";
        else if (call.kind==analysis::CallKind::return_link || call.kind==analysis::CallKind::return_exception) calls<<"runtime-continuation";
        else calls<<"unresolved-indirect";
        calls<<'\n';
    }
    write_text_file(output/"call-sites.tsv",calls.str());
    std::ostringstream pointers;
    pointers<<"site\ttarget\tsource\tuse\tcoverage\tcallee\targument_index\n";
    for (const auto& reference:prepared.pointers) {
        pointers<<hex32(reference.site)<<'\t'<<hex32(reference.target)<<'\t'
                <<(reference.argument ? "argument" : "store")<<'\t'
                <<(reference.callable ? "indirect-call-consumer" : reference.data_read ? "data-read-observed" : "unclassified")<<'\t'
                <<(prepared.discovered.instructions.contains(reference.target) ? "discovered-EE-code" : "not-established-as-EE-code")<<'\t';
        if (reference.argument) pointers<<hex32(reference.callee)<<'\t'<<reference.argument_index;
        else pointers<<"-\t-";
        pointers<<'\n';
    }
    write_text_file(output/"pointer-references.tsv",pointers.str());
    std::ostringstream passes;
    passes<<"image\tpass\tdiscovered_instructions\tnew_roots\tnew_registrations\n";
    for (const auto& pass:prepared.passes)
        passes<<pass.image<<'\t'<<pass.index<<'\t'<<pass.instructions<<'\t'<<pass.new_roots<<'\t'<<pass.new_registrations<<'\n';
    write_text_file(output/"discovery-passes.tsv",passes.str());
}}
void write_project_reports(const std::filesystem::path& output,const PreparedProject& prepared,
                           std::span<const PayloadMapping> mappings,const NativeLayout& layout_plan) {
    write_discovery_reports(output,prepared);
    std::ostringstream native_layout;
    for (const auto& record:prepared.layouts)
        native_layout<<hex32(record[0])<<' '<<hex32(record[1])<<' '<<hex32(record[2])<<' '<<hex32(record[3])<<'\n';
    write_text_file(output/"native-layout-candidates.txt",native_layout.str());
    if (!prepared.images.empty()) {
        std::ostringstream manifest;
        for (const auto& executable_image:prepared.images) {
            manifest<<"image "<<executable_image.id<<" entry "<<hex32(executable_image.entry)<<'\n';
            for (const auto& segment:executable_image.segments)
                manifest<<"  section "<<hex32(segment.address)<<" bytes "<<segment.bytes.size()<<'\n';
            for (const auto address:executable_image.patched) manifest<<"  patched instruction "<<hex32(address)<<'\n';
        }
        manifest<<"Only image 1 is initialized at launch. Other images must be loaded and validated at ExecPS2.\n";
        write_text_file(output/"executable-images.txt",manifest.str());
    }
    std::ostringstream accepted_layout;
    accepted_layout<<"Static native allocations exist from process startup; declarations validate the compiled plan.\n"
        "No mutable mappings, lookup tables, or address-space readback. Device bindings do not supply BIOS storage or device readiness.\n";
    for (const auto& view:layout_plan.aliases)
        accepted_layout<<"alias "<<hex32(view.address)<<" backing "<<hex32(view.backing)<<" bytes "<<hex32(view.bytes)<<'\n';
    for (const auto& guard:layout_plan.guards)
        accepted_layout<<"access "<<hex32(guard.address)<<" bytes "<<hex32(guard.bytes)
            <<" read "<<guard.readable<<" write "<<guard.writable<<'\n';
    accepted_layout<<"accepted declarations "<<layout_plan.declarations.size()<<'\n';
    write_text_file(output/"native-layout-plan.txt",accepted_layout.str());
    if (!mappings.empty()) {
        std::ostringstream plan;
        for (const auto& mapping:mappings) {
            plan<<mapping.source<<' '<<mapping.destination<<' '<<mapping.size;
            for (const auto offset:mapping.entry_offsets) plan<<' '<<offset;
            for (const auto& region:mapping.code_ranges) plan<<" code="<<region.offset<<':'<<region.size;
            plan<<'\n';
        }
        write_text_file(output/"payloads.txt",plan.str());
    }
    write_text_file(output/"conversion.txt","AnyPS2 EE LLVM entry-flow conversion\n"
         "Discovery includes leaf syscall-wrapper candidates and recognized startup continuations. Compiled wrappers do not implement missing services; runtime dispatch still validates them.\n"
         "Indirect destinations must already belong to the discovered code set; other targets stop.\n"
         "call-sites.tsv inventories reached transfers; compiled initial candidates do not prove complete indirect coverage.\n"
         "function-roots.tsv separately records opt-in defined ELF function metadata; it is not pointer-flow completeness proof.\n"
         "observed-call-edges.tsv lists validated native-stop targets used in this offline conversion; their other possible targets remain unresolved.\n"
         "Build with ps2-build, then run ps2-native <program.data> [operation-budget].\n"
         "Data contains private input material and must remain local. This is not game boot.\n");
}
void write_flow_reports(const std::filesystem::path& output,const PreparedProject& prepared,
                        const FlowDiagnostics& diagnostics) {
    const auto& result=diagnostics.result;
    const auto whole_code=prepared.whole_code;
    write_discovery_reports(output,prepared,true);
    std::ostringstream gaps;
    gaps<<"kind\tsite\ttarget\tevidence\n";
    for (const auto target:prepared.discovered.unresolved)
        gaps<<"unmapped-target\t-\t"<<hex32(target)<<"\treached control flow has no mapped instruction\n";
    for (const auto& call:prepared.calls) {
        if (call.kind==analysis::CallKind::service) continue;
        if (call.target || call.kind==analysis::CallKind::return_link || call.kind==analysis::CallKind::return_exception) continue;
        gaps<<"unresolved-indirect\t"<<hex32(call.pc)<<"\t-\truntime target set is not proven complete\n";
    }
    for (const auto& reference:prepared.pointers) {
        if (reference.callable || prepared.discovered.instructions.contains(reference.target)) continue;
        gaps<<"unclassified-pointer\t"<<hex32(reference.site)<<'\t'<<hex32(reference.target)<<'\t'
            <<(reference.argument ? "argument passed to " : "stored address; ");
        if (reference.argument) gaps<<hex32(reference.callee)<<" argument "<<reference.argument_index<<"; ";
        gaps<<(reference.data_read ? "data use observed, callable use not established" : "callable use not established")<<'\n';
    }
    write_text_file(output/"flow-gaps.tsv",gaps.str());
    std::ostringstream bindings;
    bindings<<"store\ttarget\tfield\tstatus\tconsumer_sites\n";
    for (const auto& gap:prepared.flow_gaps) {
        bindings<<hex32(gap.site)<<'\t'<<hex32(gap.target)<<'\t'<<hex32(gap.field)<<"\tunproven-object-identity\t";
        for (std::size_t n=0;n<gap.consumers.size();++n) {
            if (n) bindings<<',';
            bindings<<hex32(gap.consumers[n]);
        }
        bindings<<'\n';
    }
    write_text_file(output/"callback-flow-gaps.tsv",bindings.str());
    write_text_file(output/"lowering-diagnostics.txt",diagnostics.rejection.text());
    write_text_file(output/"layout-diagnostics.txt",diagnostics.layout_error);
    std::ostringstream summary;
    summary<<"known_flow="<<(result.closed() ? "closed" : "incomplete")<<'\n'
        <<"code_selection="<<(whole_code ? "whole-declared-EE-code" : "reachable-flow")<<'\n'
        <<"pointer_analysis="<<(whole_code ? "not-required-for-code-selection" : "bounded-discovery")<<'\n'
        <<"discovered_instructions="<<result.instructions<<"\ndiscovery_passes="<<result.passes
        <<"\nunmapped_targets="<<result.unmapped_targets<<"\nunresolved_indirect="<<result.unresolved_indirect
        <<"\nunclassified_pointer_references="<<result.unclassified_references
        <<"\nunproven_record_bindings="<<result.record_binding_candidates<<"\nnative_service_boundaries="<<result.service_boundaries
        <<"\nvalidated_observed_edges="<<prepared.observations.size()
        <<"\nlowering_supported="<<result.lowering_supported<<"\nlayout_supported="<<result.layout_supported
        <<"\nLLVM_emitted=0\nnative_compilation_performed=0\nruntime_executed=0\n"
        <<(whole_code ? "Every declared EE code word was selected independently of runtime targets; dynamic target values remain unproven.\n" :
            "Discovery closure means the implemented rules found no additional roots, not complete game coverage.\n")
        <<"Callback layout matches are diagnostics only and never promote a pointer to executable code.\n"
        <<"Unknown dynamic content, native service behavior, rendering and gameplay require separate validation.\n";
    write_text_file(output/"flow-verification.txt",summary.str());
}
}
