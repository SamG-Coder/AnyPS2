#include <nt/inventory.hpp>
#include "call_mapping.hpp"
#include "public_abi.hpp"
#include "report_text.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace nt::inventory {
namespace {
struct PublicName { std::string_view name,header; std::uint16_t version=0; };
PublicName public_name(const LibraryCall& call) {
    if (call.binding==CallBinding::unknown) return {};
    PublicName result;
    for (const auto& library:detail::abi_libraries) {
        if ((call.domain=="ee")!=(library.name=="kernel")) continue;
        if (library.name!=call.library || (library.version>>8)!=(call.version>>8)) continue;
        for (const auto& function:library.functions) if (function.ordinal==call.selector) {
            // Conflicting published names do not justify guessing which ABI was used.
            if (!result.name.empty() && result.name!=function.name) return {};
            result={function.name,library.header,library.version};
        }
    }
    return result;
}
std::string_view binding_name(CallBinding binding) {
    switch (binding) {
    case CallBinding::intrinsic: return "llvm-intrinsic";
    case CallBinding::native_dispatch: return "native-dispatch-candidate";
    case CallBinding::compiled_export: return "compiled-export-candidate";
    case CallBinding::registered_handler: return "registered-handler-candidate";
    case CallBinding::unresolved: return "unresolved";
    case CallBinding::unknown: return "unknown-selector";
    }
    return "unknown-selector";
}
using CallKey=std::tuple<std::string,std::string,std::uint16_t,std::uint32_t,bool>;
std::string label(const LibraryCall& call) {
    if (call.binding==CallBinding::unknown) return call.library+" unknown selector";
    if (call.domain=="ee") return call.library+" "+hex32(call.selector);
    return call.library+" "+std::to_string(call.version>>8)+"."+std::to_string(call.version&255)+" #"+std::to_string(call.selector);
}
}
std::string format_neighborhood(const iop_codegen::ir::Neighborhood& report) {
    std::ostringstream out;
    out<<"# Offline IOP call neighborhood\n\nAnchor: "<<hex32(report.anchor)<<"; range: "<<hex32(report.first)
       <<".."<<hex32(report.last)<<" (exclusive).\n\n"
       <<"Static candidates only: linear windows may include unreachable code or embedded data. Unknown arguments are not zero.\n"
       <<"Constants describe a local straight-line suffix, including the call delay slot; they are not whole-program proofs.\n"
       <<"Native binding names do not establish service support or successful execution.\n\n";
    std::map<std::string,std::size_t> groups;
    for (const auto& site:report.calls) {
        std::string identity=site.indirect ? "indirect call" : "internal "+hex32(site.target);
        if (site.import) {
            LibraryCall call; call.domain="iop"; call.library=site.import->library; call.version=site.import->version;
            call.selector=site.import->ordinal; call.binding=CallBinding::unresolved;
            identity=label(call); const auto known=public_name(call);
            if (!known.name.empty()) identity+=" "+std::string(known.name);
        }
        ++groups[identity];
        out<<"## "<<hex32(site.address)<<" -> "<<identity<<"\n\n";
        for (unsigned index=0;index<site.arguments.size();++index) {
            out<<"arg"<<index<<'=';
            if (site.arguments[index]) for (const auto value:*site.arguments[index]) out<<hex32(value)<<' ';
            else out<<'?';
            out<<'\n';
        }
        for (const auto& [offset,values]:site.stack_words) {
            out<<"stack+"<<offset<<'='; for (const auto value:values) out<<hex32(value)<<' '; out<<'\n';
        }
        for (const auto& [argument,bytes]:site.stack_arguments) {
            constexpr char digits[]="0123456789abcdef";
            out<<"arg"<<argument<<" stack bytes:";
            for (const auto value:bytes) {
                out<<' ';
                if (value) out<<digits[*value>>4]<<digits[*value&15]; else out<<"??";
            }
            out<<'\n';
        }
        if (!site.code_candidates.empty()) {
            out<<"callback/address candidates:";
            for (const auto target:site.code_candidates) out<<' '<<hex32(target);
            out<<'\n';
        }
        out<<'\n';
    }
    out<<"## Grouped call sites\n\n";
    for (const auto& [name,count]:groups) out<<count<<" x "<<name<<'\n';
    out<<"\n## Function-entry candidates\n\n";
    for (const auto address:report.function_candidates) out<<hex32(address)<<'\n';
    out<<"\n## Structural patterns\n\n";
    for (const auto& [name,count]:report.patterns) out<<name<<'='<<count<<'\n';
    out<<"\n## Undecoded words\n\n";
    for (const auto& [address,why]:report.unknown) out<<hex32(address)<<' '<<why<<'\n';
    return out.str();
}
void record_iop_calls(Inventory& inventory,std::span<const elf::IopModule> modules,
                     std::span<const std::string> sources,std::span<const iop_codegen::ir::ModuleDiagnosis> diagnoses) {
    if (modules.size()!=sources.size() || (!diagnoses.empty() && diagnoses.size()!=modules.size()))
        throw std::invalid_argument("IOP call inventory has inconsistent module metadata");
    std::vector<std::vector<elf::IopExports>> exports(modules.size());
    for (std::size_t index=0;index<modules.size();++index) {
        try { exports[index]=elf::iop_exports(modules[index]); }
        catch (const std::exception& error) {
            inventory.gaps.push_back({"iop-call-catalog","invalid export table",error.what(),sources[index],1,modules[index].base});
        }
    }
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& module=modules[index];
        std::vector<elf::IopImport> imports;
        try { imports=elf::iop_imports(module); }
        catch (const std::exception& error) {
            inventory.gaps.push_back({"iop-call-catalog","invalid import table",error.what(),sources[index],1,module.base});
            continue;
        }
        for (const auto& import:imports) {
            LibraryCall call;
            call.domain="iop"; call.library=import.library; call.source=sources[index];
            call.version=import.version; call.selector=import.ordinal; call.address=module.base+import.offset;
            call.reached=!diagnoses.empty() && diagnoses[index].program.instructions.contains(*call.address);
            for (std::size_t provider=0;provider<modules.size();++provider) for (const auto& table:exports[provider]) {
                if (table.name==import.library && (table.version>>8)==(import.version>>8) &&
                    import.ordinal<table.functions.size() && table.functions[import.ordinal]) {
                    call.providers.push_back({sources[provider],table.version,
                        !diagnoses.empty() && diagnoses[provider].convertible()});
                }
            }
            auto native=iop_codegen::ir::native_import_operation(import);
            if (import.library=="romdrv" && !call.providers.empty()) native.reset();
            if (native) call.binding=*native==iop_codegen::ir::Operation::native_import ?
                CallBinding::native_dispatch : CallBinding::intrinsic;
            else call.binding=call.providers.empty() ? CallBinding::unresolved : CallBinding::compiled_export;
            inventory.calls.push_back(std::move(call));
        }
    }
}
void write_call_reports(const Inventory& inventory,const std::filesystem::path& output) {
    using detail::quote;
    using detail::table_text;
    std::map<CallKey,std::vector<const LibraryCall*>> groups;
    for (const auto& call:inventory.calls)
        groups[{call.domain,call.library,call.version,call.selector,call.binding==CallBinding::unknown}].push_back(&call);
    std::map<std::string_view,std::size_t> categories;
    std::size_t named=0,sites=0;
    for (const auto& [key,uses]:groups) {
        const auto name=public_name(*uses.front());
        named+=!name.name.empty();
        ++categories[detail::mapping_for(*uses.front(),name.name).category];
        for (const auto* use:uses) sites+=use->sites;
    }
    std::ostringstream json,markdown;
    json<<"{\n  \"schema\":\"anyps2-library-calls-v1\",\n  \"unique_calls\":"<<groups.size()
        <<",\n  \"named_calls\":"<<named<<",\n  \"declared_sites\":"<<sites
        <<",\n  \"runtime_verified\":false,\n  \"scope\":\"All parsed IOP import stubs and statically recognized EE syscalls, including leaf-wrapper candidates. Not all dynamic calls, RPC method selectors, MMIO accesses or unidentified code.\",\n"
        <<"  \"name_evidence\":\"Public PS2SDK major-ABI names are hints, not proven signatures or behavioral compatibility.\",\n"
        <<"  \"binding_scope\":\"Archive-local conversion candidates. Providers loaded by another image or reset are not assumed registered.\",\n"
        <<"  \"calls\":[\n";
    markdown<<"# Library-call inventory\n\nUnique call identities: **"<<groups.size()<<"**; public names: **"<<named
        <<"**; declared sites/static candidates: **"<<sites<<"**. These are not runtime invocation counts.\n\n"
        <<"Includes parsed IOP import tables and discovered EE syscall candidates, even in units that do not compile. "
        <<"Dynamic calls, RPC method selectors, MMIO and unidentified code require further discovery. "
        <<"A public name or matching export is not proof of a working replacement. No provider is automatically installed.\n\n"
        <<"Bindings reflect archive-local conversion. Providers loaded by another image or reset are not assumed registered.\n\n"
        <<"The JSON companion preserves every source, import address, binding candidate and provider. "
        <<"Import addresses use the inventory placement, not a runtime load address. "
        <<"A false reached flag means reachability is unproven, not that the call is unused.\n\n"
        <<"| Native domain | Unique call identities |\n| --- | ---: |\n";
    for (const auto& [category,count]:categories) markdown<<"| "<<category<<" | "<<count<<" |\n";
    std::set<std::string_view> emitted;
    std::size_t index=0;
    for (const auto& [key,uses]:groups) {
        const auto& call=*uses.front();
        const auto name=public_name(call);
        const auto& mapping=detail::mapping_for(call,name.name);
        if (emitted.insert(mapping.category).second) {
            markdown<<"\n## "<<mapping.category<<"\n\nCurrent: "<<mapping.current<<". Candidate: **"<<mapping.candidate
                <<"** ("<<mapping.license<<").\n\nTarget: "<<mapping.target<<".\n\nContract: "<<mapping.contract
                <<"\n\nOptimization: "<<mapping.optimization<<"\n";
            if (!mapping.reference.empty()) markdown<<"\n[Reference]("<<mapping.reference<<")\n";
        }
        if (index++) json<<",\n";
        json<<"    {\"domain\":"<<quote(call.domain)<<",\"library\":"<<quote(call.library)<<",\"version\":"<<call.version
            <<",\"selector\":";
        if (call.binding==CallBinding::unknown) json<<"null"; else json<<call.selector;
        json<<",\"name\":"<<quote(name.name)<<",\"public_header\":"<<quote(name.header)<<",\"public_version\":"<<name.version
            <<",\"mapping\":{\"category\":"<<quote(mapping.category)<<",\"current\":"<<quote(mapping.current)
            <<",\"candidate\":"<<quote(mapping.candidate)<<",\"license\":"<<quote(mapping.license)
            <<",\"target\":"<<quote(mapping.target)<<",\"contract\":"<<quote(mapping.contract)
            <<",\"optimization\":"<<quote(mapping.optimization)<<",\"reference\":"<<quote(mapping.reference)
            <<",\"replacement_verified\":false},\"uses\":[";
        for (std::size_t occurrence=0;occurrence<uses.size();++occurrence) {
            const auto& use=*uses[occurrence];
            if (occurrence) json<<',';
            json<<"{\"source\":"<<quote(use.source)<<",\"sites\":"<<use.sites<<",\"address\":";
            if (use.address) json<<*use.address; else json<<"null";
            json<<",\"reached\":"<<(use.reached?"true":"false")<<",\"binding\":"<<quote(binding_name(use.binding))<<",\"providers\":[";
            for (std::size_t n=0;n<use.providers.size();++n) {
                if (n) json<<',';
                const auto& provider=use.providers[n];
                json<<"{\"source\":"<<quote(provider.source)<<",\"version\":"<<provider.version
                    <<",\"convertible\":"<<(provider.convertible?"true":"false")<<'}';
            }
            json<<"]}";
        }
        json<<"]}";
    }
    json<<"\n  ]\n}\n";
    markdown<<"\n## All identified calls\n\n| Call | Public name hint | Binding candidates | Declared sites | Sources | Native domain |\n"
        <<"| --- | --- | --- | ---: | ---: | --- |\n";
    for (const auto& [key,uses]:groups) {
        const auto& call=*uses.front();
        const auto name=public_name(call);
        std::size_t count=0;
        std::set<std::string_view> bindings,sources;
        for (const auto* use:uses) { count+=use->sites; bindings.insert(binding_name(use->binding)); sources.insert(use->source); }
        markdown<<"| "<<table_text(call.domain+" "+label(call))<<" | "<<table_text(name.name.empty()?"unknown":name.name)<<" | ";
        bool separator=false;
        for (const auto binding:bindings) { if (separator) markdown<<", "; markdown<<binding; separator=true; }
        markdown<<" | "<<count<<" | "<<sources.size()<<" | "<<detail::mapping_for(call,name.name).category<<" |\n";
    }
    detail::save(output/"library-calls.json",json.str());
    detail::save(output/"library-calls.md",markdown.str());
}
}
