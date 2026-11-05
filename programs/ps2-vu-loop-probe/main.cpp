#include <nt/render_loop.hpp>
#include <nt/loop_structure.hpp>
#include <nt/loop_pipeline.hpp>
#include <nt/element_packet.hpp>
#include <nt/element_range.hpp>
#include <nt/vector_region_codegen.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
const char* access_name(nt::render::AccessKind kind) {
    switch (kind) {
    case nt::render::AccessKind::load_vector: return "load";
    case nt::render::AccessKind::store_vector: return "store";
    case nt::render::AccessKind::load_integer: return "load-integer";
    case nt::render::AccessKind::store_integer: return "store-integer";
    }
    return "?";
}
std::string base_name(int base) {
    if (base<0) return "abs";
    if (base==16) return "top";
    if (base==17) return "itop";
    return "vi"+std::to_string(base);
}
// One line per loop: shape, induction, streams, carried state and blockers.
void report_structures(const std::vector<nt::vu::Pair>& program,unsigned unit,std::ostream& out) {
    for (const auto& region:nt::render::find_loop_regions(program,unit)) {
        const auto first=region.head-program.front().address,last=region.branch+1-program.front().address;
        if (last>=program.size()) continue;
        const auto body=std::span(program).subspan(first,last-first+1);
        nt::render::LoopStructure loop;
        try { loop=nt::render::analyze_loop_structure(body,unit); }
        catch (const std::exception& error) {
            out<<"loop "<<region.head<<"-"<<region.branch+1<<" malformed: "<<error.what()<<'\n';
            continue;
        }
        out<<"loop "<<loop.head<<"-"<<loop.branch+1<<" pairs="<<loop.pairs;
        if (loop.counter) out<<" counter=vi"<<*loop.counter<<" step="<<loop.steps[*loop.counter];
        out<<" induction=[";
        for (unsigned n=1;n<16;++n) if (loop.affine_update[n] && loop.steps[n]) out<<" vi"<<n<<":"<<loop.steps[n];
        out<<" ] streams=[";
        for (const auto& access:loop.accesses) {
            out<<' '<<access_name(access.kind)<<":"<<access.reg;
            if (!access.address.known) out<<"@?";
            else out<<"@"<<base_name(access.address.base)<<(access.address.offset>=0 ? "+" : "")<<access.address.offset<<"/"<<access.stride;
        }
        out<<" ] carried=[";
        for (unsigned n=1;n<32;++n) if (loop.carried_vectors[n]!=nt::render::CarriedValue::none)
            out<<" vf"<<n<<(loop.carried_vectors[n]==nt::render::CarriedValue::prefetched ? ":load" : ":computed");
        out<<" ] invariant=[";
        for (unsigned n=1;n<32;++n) if (loop.invariant_vectors[n]) out<<" vf"<<n;
        out<<" ] kicks="<<loop.graphics_kicks<<" clip="<<loop.clip_writes<<"/"<<loop.clip_reads
           <<" flags="<<loop.mac_reads+loop.status_reads<<" q="<<loop.uses_quotient<<" p="<<loop.uses_p<<" blockers=[";
        for (std::size_t n=0;n<loop.blockers.size();++n) out<<(n ? "; " : "")<<loop.blockers[n];
        const auto recovered=nt::render::recover_pipelined_loop(body,unit,loop);
        if (recovered.loop) {
            std::array<unsigned,16> operations{};
            for (const auto& node:recovered.loop->nodes)
                if (node.kind==nt::render::ElementNodeKind::operation && node.a<operations.size()) ++operations[node.a];
            out<<"] pipeline=[depth="<<recovered.loop->depth<<" stores="<<recovered.loop->stores.size()
               <<" nodes="<<recovered.loop->nodes.size()<<" operations=";
            for (unsigned n=0;n<operations.size();++n) if (operations[n]) out<<n<<":"<<operations[n]<<",";
            out<<"]\n";
        }
        else out<<"] pipeline=[no: "<<recovered.reason<<"]\n";
    }
}
template<class T> T number(std::string_view text,int base=10) {
    T value=0;
    if (base==16 && text.starts_with("0x")) text.remove_prefix(2);
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Invalid numeric argument");
    return value;
}
unsigned decimal(std::string_view text) { return number<unsigned>(text); }
}
int main(int argc,char** argv) {
    if (argc==7 && std::string_view(argv[1])=="--listing") {
        // Decoded operations of a pair range (diagnostic; no instruction text is
        // emitted into code): upper operation and destination, lower operation,
        // its registers, address register/update, offset and branch target.
        try {
            const auto unit=decimal(argv[3]),destination=decimal(argv[4]),first=decimal(argv[5]),count=decimal(argv[6]);
            const auto program=nt::vu::frame(nt::read_file(argv[2],unit ? 16384:4096),unit,destination);
            for (const auto& pair:program) {
                if (pair.address<first || pair.address>=first+count) continue;
                const auto upper=nt::vu::decode_upper(pair.upper);
                const auto lower=nt::vu::decode_lower(pair,unit);
                std::cout<<pair.address<<"\t"<<nt::vu::name(upper.operation)<<" vf"<<upper.result<<" mask="<<upper.mask
                         <<"\t"<<nt::vu::name(lower.operation)<<" r="<<lower.result.index<<" l="<<lower.left.index
                         <<" a=vi"<<lower.address_register<<(lower.address_update==nt::vu::AddressUpdate::increment ? "++" :
                            lower.address_update==nt::vu::AddressUpdate::decrement ? "--" : "")
                         <<" off="<<lower.offset;
                if (lower.operation==nt::vu::LowerOperation::branch && lower.transfer.target) std::cout<<" target="<<*lower.transfer.target;
                if (pair.end) std::cout<<" E";
                std::cout<<'\n';
            }
            return std::cout ? 0:1;
        } catch (const std::exception& error) { std::cerr<<"Listing: "<<error.what()<<'\n'; return 1; }
    }
    if (argc==6 && std::string_view(argv[1])=="--regions") {
        // Every loop of a whole-entry image: accepted regions as separate LLVM
        // modules (boundary, elements, binding), a head-to-binding table module,
        // and a manifest recording each loop's acceptance or rejection reason.
        try {
            const auto unit=decimal(argv[3]),destination=decimal(argv[4]);
            const std::filesystem::path directory=argv[5];
            if (std::filesystem::exists(directory)) throw std::invalid_argument("Region output directory already exists");
            const auto program=nt::vu::frame(nt::read_file(argv[2],unit ? 16384:4096),unit,destination);
            std::vector<unsigned> entries;
            for (const auto& pair:program) entries.push_back(pair.address);
            std::filesystem::create_directory(directory);
            std::ostringstream manifest,table;
            manifest<<"head\tresult\treason\n";
            std::vector<std::pair<unsigned,std::string>> accepted;
            for (const auto& loop:nt::render::find_loop_regions(program,unit)) {
                try {
                    const auto region=nt::render::analyze_vector_region(program,unit,entries,loop.head);
                    const auto symbol="nt_vu"+std::to_string(unit)+"_region_"+std::to_string(loop.head);
                    std::ostringstream boundary,elements,binding;
                    nt::render::emit_vector_region_llvm(boundary,elements,binding,region,symbol);
                    for (const auto& [part,text]:{std::pair{"boundary",boundary.str()},{"elements",elements.str()},{"binding",binding.str()}})
                        nt::write_text_file(directory/(symbol+"."+part+".ll"),text);
                    accepted.push_back({loop.head,symbol});
                    manifest<<loop.head<<"\taccepted\t\n";
                } catch (const std::invalid_argument& error) {
                    manifest<<loop.head<<"\trejected\t"<<error.what()<<"\n";
                }
            }
            const auto prefix="nt_vu"+std::to_string(unit)+"_region";
            for (const auto& [head,symbol]:accepted) table<<"@"<<symbol<<" = external constant i8\n";
            table<<"@"<<prefix<<"_table = private constant ["<<accepted.size()<<" x { i32, ptr }] [";
            for (std::size_t i=0;i<accepted.size();++i)
                table<<(i ? ", " : "")<<"{ i32, ptr } { i32 "<<accepted[i].first<<", ptr @"<<accepted[i].second<<" }";
            table<<"]\ndefine ptr @"<<prefix<<"s() {\n  ret ptr @"<<prefix<<"_table\n}\n"
                 <<"define i32 @"<<prefix<<"_count() {\n  ret i32 "<<accepted.size()<<"\n}\n";
            nt::write_text_file(directory/(prefix+"_table.ll"),table.str());
            nt::write_text_file(directory/"regions.tsv",manifest.str());
            std::cout<<"Emitted "<<accepted.size()<<" vector regions; LLVM compilation and linking remain separate stages.\n";
            return 0;
        } catch (const std::exception& error) { std::cerr<<"Vector regions: "<<error.what()<<'\n'; return 1; }
    }
    if (argc>=8 && std::string_view(argv[1])=="--region") {
        try {
            const auto unit=decimal(argv[3]),destination=decimal(argv[4]),head=decimal(argv[5]);
            const std::string_view part=argv[6];
            if (part!="boundary" && part!="elements" && part!="binding")
                throw std::invalid_argument("Expected boundary, elements or binding");
            std::vector<unsigned> entries;
            for (int i=7;i<argc;++i) entries.push_back(decimal(argv[i]));
            const auto program=nt::vu::frame(nt::read_file(argv[2],unit ? 16384:4096),unit,destination);
            const auto region=nt::render::analyze_vector_region(program,unit,entries,head);
            std::ostringstream boundary,elements,binding;
            nt::render::emit_vector_region_llvm(boundary,elements,binding,region,"nt_render_region");
            std::cout<<(part=="boundary" ? boundary.str():part=="elements" ? elements.str():binding.str());
            return std::cout ? 0:1;
        } catch (const std::exception& error) { std::cerr<<"Vector region probe: "<<error.what()<<'\n'; return 1; }
    }
    const bool binding_mode=argc==10 && std::string_view(argv[1])=="--element-binding";
    if ((argc==7 && std::string_view(argv[1])=="--element") || binding_mode) {
        try {
            const auto unit=decimal(argv[3]),destination=decimal(argv[4]),head=decimal(argv[5]);
            const std::string_view target=argv[6];
            if (!binding_mode && target!="cpu" && target!="vulkan" && target!="guard") throw std::invalid_argument("Expected cpu, vulkan or guard");
            const auto program=nt::vu::frame(nt::read_file(argv[2],unit ? 16384 : 4096),unit,destination);
            for (const auto& region:nt::render::find_loop_regions(program,unit)) {
                if (region.head!=head) continue;
                const auto body=std::span(program).subspan(region.head-program.front().address,region.branch+2-region.head);
                const auto recovered=nt::render::recover_pipelined_loop(body,unit,nt::render::analyze_loop_structure(body,unit));
                if (!recovered.loop) throw std::invalid_argument("Loop not recovered: "+recovered.reason);
                if (binding_mode) {
                    const auto binding=nt::render::bind_element_packet(*recovered.loop,decimal(argv[6]),number<std::int32_t>(argv[7]),
                        number<std::uint64_t>(argv[8],16),number<std::uint64_t>(argv[9],16));
                    nt::render::emit_element_packet_llvm(std::cout,binding,"nt_render_vertex_layout");
                } else if (target=="guard") {
                    const auto guard=nt::render::analyze_element_range(body,unit);
                    nt::render::emit_element_range_llvm(std::cout,guard,"nt_render_element_guard");
                } else nt::render::emit_element_kernel_llvm(std::cout,*recovered.loop,unit,target=="vulkan");
                return std::cout ? 0 : 1;
            }
            throw std::invalid_argument("No loop starts at that pair");
        } catch (const std::exception& error) { std::cerr<<"Element kernel probe: "<<error.what()<<'\n'; return 1; }
    }
    if (argc==5 && std::string_view(argv[1])=="--structure") {
        try {
            const auto unit=decimal(argv[3]),destination=decimal(argv[4]);
            const auto bytes=nt::read_file(argv[2],unit ? 16384 : 4096);
            report_structures(nt::vu::frame(bytes,unit,destination),unit,std::cout);
            return std::cout ? 0 : 1;
        } catch (const std::exception& error) { std::cerr<<"Loop structure probe: "<<error.what()<<'\n'; return 1; }
    }
    if (argc!=6 || std::string_view(argv[5])!="--native-f32") {
        std::cerr<<"Usage: ps2-vu-loop-probe <isolated-loop.bin> <unit:0|1> <destination-index> "
                   "<manifest|cpu-llvm|spirv-llvm> --native-f32\n"
                   "       ps2-vu-loop-probe --region <image> <unit> <destination> <head> boundary|elements|binding <entries...>\n"
                   "       ps2-vu-loop-probe --regions <whole-entry-image> <unit> <destination> <new-output-directory>\n"
                   "       ps2-vu-loop-probe --listing <image> <unit> <destination> <first-pair> <count>\n"
                   "       ps2-vu-loop-probe --structure <program-image.bin> <unit:0|1> <destination-index>\n"
                   "       ps2-vu-loop-probe --element <program-image.bin> <unit> <destination> <loop-head> cpu|vulkan|guard\n"
                   "       ps2-vu-loop-probe --element-binding <program-image.bin> <unit> <destination> <loop-head> "
                   "<base-register> <first-row> <tag-low-hex> <registers-hex>\n";
        return 2;
    }
    try {
        const auto unit=decimal(argv[2]),destination=decimal(argv[3]);
        const auto bytes=nt::read_file(argv[1],2048);
        const auto loop=nt::render::analyze_vertex_loop(nt::vu::frame(bytes,unit,destination),unit);
        std::ostringstream output;
        const std::string_view format=argv[4];
        if (format=="manifest") nt::render::write_manifest(output,loop);
        else if (format=="cpu-llvm") nt::render::emit_cpu_llvm(output,loop,nt::render::NumericPolicy::native_float32);
        else if (format=="spirv-llvm") nt::render::emit_spirv_llvm(output,loop,nt::render::NumericPolicy::native_float32);
        else throw std::invalid_argument("Unknown output format");
        std::cout<<output.str();
        if (!std::cout) throw std::runtime_error("Cannot write probe output");
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"Vertex loop probe: "<<error.what()<<'\n'; return 1;
    }
}
