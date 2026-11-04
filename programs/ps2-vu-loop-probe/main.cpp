#include <nt/render_loop.hpp>
#include <nt/loop_structure.hpp>
#include <charconv>
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
        out<<"]\n";
    }
}
unsigned decimal(std::string_view text) {
    unsigned value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Expected a decimal argument");
    return value;
}
}
int main(int argc,char** argv) {
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
                   "       ps2-vu-loop-probe --structure <program-image.bin> <unit:0|1> <destination-index>\n";
        return 2;
    }
    try {
        const auto number=[](std::string_view text) {
            unsigned value=0;
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
                throw std::invalid_argument("Expected a decimal argument");
            return value;
        };
        const auto unit=number(argv[2]),destination=number(argv[3]);
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
