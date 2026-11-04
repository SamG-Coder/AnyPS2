#include <nt/render_codegen.hpp>
#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

int main(int argc,char** argv) {
    if (argc<7) {
        std::cerr<<"Usage: ps2-vu-render-probe <private-region.bin> <unit:0|1> <destination> <output-vf> "
            "<manifest|cpu-llvm|spirv-llvm> --native-f32 [uniform-vf ...]\n"
            "Research only: no automatic region extraction, GPU dispatch or runtime installation.\n";
        return 2;
    }
    try {
        const auto number=[](std::string_view text) {
            unsigned value=0;
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
                throw std::invalid_argument("Expected an unsigned decimal argument");
            return value;
        };
        if (std::string_view(argv[6])!="--native-f32")
            throw std::invalid_argument("Native float32 is a numerical change; --native-f32 must be explicit");
        const std::string_view format=argv[5];
        if (format!="manifest" && format!="cpu-llvm" && format!="spirv-llvm")
            throw std::invalid_argument("Unknown output format");
        const auto unit=number(argv[2]),destination=number(argv[3]),output=number(argv[4]);
        const auto bytes=nt::read_file(argv[1],256*8);
        const auto pairs=nt::vu::frame(bytes,unit,destination);
        std::vector<unsigned> uniforms;
        for (int n=7;n<argc;++n) uniforms.push_back(number(argv[n]));
        const std::array<nt::render::Lane,4> outputs{{
            {nt::render::RegisterFile::vector,output,0},{nt::render::RegisterFile::vector,output,1},
            {nt::render::RegisterFile::vector,output,2},{nt::render::RegisterFile::vector,output,3}}};
        const auto kernel=nt::render::lift_vu_arithmetic(pairs,unit,outputs,uniforms);
        if (format=="manifest") nt::render::write_manifest(std::cout,kernel);
        else if (format=="cpu-llvm") nt::render::emit_cpu_llvm(std::cout,kernel,nt::render::NumericPolicy::native_float32);
        else nt::render::emit_spirv_llvm(std::cout,kernel,nt::render::NumericPolicy::native_float32);
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"Render probe: "<<error.what()<<'\n'; return 1;
    }
}
