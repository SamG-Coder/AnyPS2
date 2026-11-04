#include "render_loop_fixture.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Unsafe loop or binding was accepted");
}
void run() {
    using namespace nt::render;
    for (unsigned variant=0;variant<2;++variant) {
        const auto c=render_fixture::configuration(variant);
        const auto loop=render_fixture::loop(variant);
        const auto sizes=layout(loop.kernel());
        require(loop.source_pairs()==18 && loop.counter()==c.count,"Loop extent/count recovery failed");
        require(loop.reads().size()==5 && sizes.vertex_words==4 && sizes.uniform_words==16 && sizes.output_words==4,
            "Automatic binding-rate recovery failed");
        require(loop.kernel().arithmetic.expressions.size()==28,"Matrix graph unexpectedly changed");
        require(loop.write().stride_rows==c.output_stride,"Output induction was not recovered");
        const auto memory=std::vector<std::uint32_t>(4096,0x3f800000U);
        auto registers=render_fixture::integers(variant,17);
        const auto bindings=bind_vertex_loop(loop,registers,memory);
        require(bindings.parameters.size()==21 && bindings.parameters[0]==17 && bindings.output_words==68,
            "Wrong parameter/output allocation contract");
        require(bindings.source_output_row==600 && bindings.source_output_stride==c.output_stride,"Source output mapping lost");
        for (unsigned n=0;n<loop.kernel().inputs.size();++n) {
            const auto& input=loop.kernel().inputs[n];
            require(bindings.parameters[n+1]==(input.rate==InputRate::uniform ? 0x3f800000U : 64+input.source.component),
                "Binding unexpectedly repacked vertices or misread a uniform");
        }
        registers[c.count]=0;
        rejects([&] { (void)bind_vertex_loop(loop,registers,memory); });
        registers=render_fixture::integers(variant,17); registers[c.output_base]=registers[c.input_base];
        rejects([&] { (void)bind_vertex_loop(loop,registers,memory); });
        registers=render_fixture::integers(variant,17); registers[c.output_base]=registers[c.uniform_base];
        rejects([&] { (void)bind_vertex_loop(loop,registers,memory); });
        registers=render_fixture::integers(variant,17); registers[c.input_base]=65530;
        rejects([&] { (void)bind_vertex_loop(loop,registers,memory); });
        registers=render_fixture::integers(variant,17);
        rejects([&] { (void)bind_vertex_loop(loop,registers,std::span(memory).first(256)); });
        rejects([&] { (void)bind_vertex_loop(loop,registers,std::span(memory).first(4095)); });
        registers[0]=1;
        rejects([&] { (void)bind_vertex_loop(loop,registers,memory); });
        std::ostringstream cpu,gpu,manifest;
        emit_cpu_llvm(cpu,loop,NumericPolicy::native_float32);
        emit_spirv_llvm(gpu,loop,NumericPolicy::native_float32); write_manifest(manifest,loop);
        for (const auto* token:{"%state","nt_ee_","nt_gpu_"," fast "," contract "}) {
            require(cpu.str().find(token)==std::string::npos,"CPU retained execution machinery or unsafe arithmetic");
            require(gpu.str().find(token)==std::string::npos,"GPU retained execution machinery or unsafe arithmetic");
        }
        require(cpu.str().find("%item = phi i32")!=std::string::npos,"CPU output does not compile the whole batch");
        require(gpu.str().find("i32 0, i32 2, i32 1")!=std::string::npos,"Parameter descriptor binding is missing");
        require(manifest.str().find("\"installable\": false")!=std::string::npos,"Loop lost its integration gate");
    }
    const auto reject_program=[](const nt::Bytes& bytes) {
        rejects([&] { (void)analyze_vertex_loop(nt::vu::frame(bytes,1,0),1); });
    };
    auto changed=render_fixture::program(0);
    render_fixture::replace(changed,0,false,render_fixture::load(1,3,0)&~(1U<<21)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,1,false,render_fixture::load(1,3,1)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,13,false,render_fixture::add(4,-2)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,12,false,render_fixture::lower_nop); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,17,false,render_fixture::add(1,1)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,16,false,(41U<<25)|(4U<<11)|0x7f0U); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,0,false,render_fixture::load(1,4,0)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,6,false,render_fixture::load(7,3,0)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,8,false,0x800006fcU); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,6,true,render_fixture::upper(61,2,1,5)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,6,true,render_fixture::upper(60,6,30,5)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,15,false,render_fixture::add(9,1)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,10,false,render_fixture::store(6,2,0)&~(1U<<21)); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,0,true,render_fixture::upper_nop|0x40000000U); reject_program(changed);
    changed=render_fixture::program(0); render_fixture::replace(changed,5,true,render_fixture::upper(42,6,1,5,0)); reject_program(changed);
    changed=render_fixture::program(0); changed.resize(changed.size()-8); reject_program(changed);
    auto pairs=nt::vu::frame(render_fixture::program(0),1,0); ++pairs[3].address;
    rejects([&] { (void)analyze_vertex_loop(pairs,1); });
    const auto small=render_fixture::loop(0,0);
    const auto memory=std::vector<std::uint32_t>(1024);
    require(bind_vertex_loop(small,render_fixture::integers(0,32,140),memory).output_words==128,"VU0 bounds rejected a valid slice");
    std::cout<<"Vertex loop discovery, inference, guarded binding and rejection checks passed\n";
}
void emit(const std::string& prefix,unsigned variant) {
    const auto loop=render_fixture::loop(variant);
    std::ostringstream cpu,gpu,manifest;
    nt::render::emit_cpu_llvm(cpu,loop,nt::render::NumericPolicy::native_float32);
    nt::render::emit_spirv_llvm(gpu,loop,nt::render::NumericPolicy::native_float32);
    nt::render::write_manifest(manifest,loop);
    nt::write_text_file(prefix+".cpu.ll",cpu.str()); nt::write_text_file(prefix+".gpu.ll",gpu.str());
    nt::write_text_file(prefix+".json",manifest.str()); nt::write_file(prefix+".bin",render_fixture::program(variant));
}
}
int main(int argc,char** argv) {
    try {
        if (argc==4 && std::string_view(argv[1])=="--emit" && (std::string_view(argv[3])=="0" || std::string_view(argv[3])=="1")) {
            emit(argv[2],argv[3][0]-'0'); return 0;
        }
        if (argc!=1) throw std::invalid_argument("Invalid test argument");
        run(); return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
