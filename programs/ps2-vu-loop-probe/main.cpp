#include <nt/render_loop.hpp>
#include <charconv>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

int main(int argc,char** argv) {
    if (argc!=6 || std::string_view(argv[5])!="--native-f32") {
        std::cerr<<"Usage: ps2-vu-loop-probe <isolated-loop.bin> <unit:0|1> <destination-index> "
                   "<manifest|cpu-llvm|spirv-llvm> --native-f32\n";
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
