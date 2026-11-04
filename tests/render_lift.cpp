#include <nt/render_codegen.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using nt::render::Lane;
using nt::render::RegisterFile;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) {
    bool rejected=false;
    try { action(); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"Unsupported input was accepted");
}
void word(nt::Bytes& bytes,std::uint32_t value) {
    for (unsigned n=0;n<4;++n) bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
std::uint32_t upper(unsigned function,unsigned destination,unsigned left,unsigned right,unsigned mask=15) {
    return (mask<<21)|(right<<16)|(left<<11)|(destination<<6)|function;
}
nt::Bytes encode(std::initializer_list<std::uint32_t> operations,std::uint32_t lower=0x8000033cU) {
    nt::Bytes result;
    result.reserve(operations.size()*8);
    for (auto operation:operations) { word(result,lower); word(result,operation); }
    return result;
}
std::array<Lane,4> vector(unsigned index) {
    return {{{RegisterFile::vector,index,0},{RegisterFile::vector,index,1},
             {RegisterFile::vector,index,2},{RegisterFile::vector,index,3}}};
}
nt::Bytes matrix() {
    // Original synthetic four-column transform: ACC = c0*x + c1*y + c2*z;
    // vf6 = ACC + c3*w. This is not extracted from a game or SDK fixture.
    return encode({upper(60,6,1,5),upper(61,2,2,5),upper(62,2,3,5),upper(11,6,4,5)});
}
nt::render::Kernel matrix_kernel() {
    const auto pairs=nt::vu::frame(matrix(),1,0);
    const std::array uniforms{1U,2U,3U,4U};
    return nt::render::lift_vu_arithmetic(pairs,1,vector(6),uniforms);
}
std::vector<float> evaluate(const nt::render::Kernel& kernel,const std::function<float(Lane)>& input) {
    // Test-only graph oracle. Production output contains compiled operations.
    std::vector<float> values;
    for (const auto& binding:kernel.inputs) values.push_back(input(binding.source));
    for (const auto& expression:kernel.arithmetic.expressions) {
        using O=nt::gpu::Operation;
        float value=0;
        switch (expression.operation) {
        case O::constant: value=std::bit_cast<float>(expression.bits); break;
        case O::add: value=values[expression.left]+values[expression.right]; break;
        case O::subtract: value=values[expression.left]-values[expression.right]; break;
        case O::multiply: value=values[expression.left]*values[expression.right]; break;
        }
        values.push_back(value);
    }
    std::vector<float> output;
    for (auto value:kernel.arithmetic.outputs) output.push_back(values[value]);
    return output;
}
void run() {
    const auto kernel=matrix_kernel(); const auto sizes=nt::render::layout(kernel);
    require(sizes.vertex_words==4 && sizes.uniform_words==16 && sizes.output_words==4,"Matrix binding rates are incorrect");
    require(kernel.arithmetic.expressions.size()==28 && kernel.inputs.size()==20,"Dead input/constant pruning failed");
    require(kernel.source_pairs==4,"Source extent was lost");
    for (unsigned n=0;n<16;++n) require(kernel.inputs[n].rate==nt::render::InputRate::uniform,"Uniform ordering changed");
    auto pairs=nt::vu::frame(encode({upper(42,6,1,2),upper(42,7,1,2),upper(40,8,3,4)}),1,0);
    const auto a=vector(6),b=vector(7);
    std::vector<Lane> outputs(a.begin(),a.end()); outputs.insert(outputs.end(),b.begin(),b.end());
    const auto common=nt::render::lift_vu_arithmetic(pairs,1,outputs);
    require(common.arithmetic.expressions.size()==4 && common.inputs.size()==8,"CSE or dead arithmetic pruning failed");
    require(std::equal(common.arithmetic.outputs.begin(),common.arithmetic.outputs.begin()+4,
        common.arithmetic.outputs.begin()+4),"Equivalent outputs were not shared");
    pairs=nt::vu::frame(encode({upper(24,1,2,1)}),1,0);
    const auto broadcast=nt::render::lift_vu_arithmetic(pairs,1,vector(1));
    auto result=evaluate(broadcast,[](Lane source) { return source.index==1 ? 3.0f : float(source.component+1); });
    require(result==std::vector<float>({3,6,9,12}),"Broadcast observed an earlier lane write");
    pairs=nt::vu::frame(encode({upper(40,6,1,2,8)}),1,0);
    const auto masked=nt::render::lift_vu_arithmetic(pairs,1,vector(6));
    result=evaluate(masked,[](Lane source) { return source.index==6 ? float(50+10*source.component) : float(source.index); });
    require(result==std::vector<float>({3,60,70,80}),"Masked destination lanes were discarded");
    pairs=nt::vu::frame(encode({upper(15,6,1,2)}),1,0);
    const auto subtract=nt::render::lift_vu_arithmetic(pairs,1,vector(6));
    result=evaluate(subtract,[](Lane source) { return source.file==RegisterFile::accumulator ? 20.0f : float(source.index+1); });
    require(result==std::vector<float>({14,14,14,14}),"Accumulator subtraction order changed");
    require(std::count_if(subtract.inputs.begin(),subtract.inputs.end(),[](const auto& input) {
        return input.source.file==RegisterFile::accumulator; })==4,"Live accumulator input was not captured");
    pairs=nt::vu::frame(encode({upper(42,0,2,3),upper(40,6,1,0)}),1,0);
    const auto constant=nt::render::lift_vu_arithmetic(pairs,1,vector(6));
    result=evaluate(constant,[](Lane) { return 2.0f; });
    require(result==std::vector<float>({2,2,2,3}),"vf0 constants were overwritten");
    pairs=nt::vu::frame(encode({upper(42,6,1,0)}),1,0);
    const auto zero=nt::render::lift_vu_arithmetic(pairs,1,vector(6));
    require(zero.inputs.size()==4,"Unsafe x*0 algebraic rewrite removed a live input");
    // Every boundary below must be refused even if its result looks dead locally.
    const std::array lower_operations{0x00000000U,0x02000000U,0x800006fcU,0x40000000U,
        0x30000000U,0x28000000U,0x800003bcU,0x80000330U};
    for (auto lower:lower_operations) {
        auto unsupported=nt::vu::frame(encode({upper(42,6,1,2)},lower),1,0);
        rejects([&] { (void)nt::render::lift_vu_arithmetic(unsupported,1,vector(6)); });
    }
    for (auto flag:{0x40000000U,0x20000000U,0x10000000U,0x08000000U,0x80000000U}) {
        auto unsupported=nt::vu::frame(encode({upper(42,6,1,2)|flag}),1,0);
        rejects([&] { (void)nt::render::lift_vu_arithmetic(unsupported,1,vector(6)); });
    }
    for (auto operation:{upper(43,6,1,2),upper(28,6,1,2)}) {
        auto unsupported=nt::vu::frame(encode({operation}),1,0);
        rejects([&] { (void)nt::render::lift_vu_arithmetic(unsupported,1,vector(6)); });
    }
    pairs=nt::vu::frame(matrix(),1,0); ++pairs[1].address;
    rejects([&] { (void)nt::render::lift_vu_arithmetic(pairs,1,vector(6)); });
    pairs=nt::vu::frame(matrix(),1,0);
    rejects([&] { (void)nt::render::lift_vu_arithmetic(pairs,2,vector(6)); });
    rejects([&] { (void)nt::render::lift_vu_arithmetic(pairs,1,vector(32)); });
    const std::array invalid_uniform{0U};
    rejects([&] { (void)nt::render::lift_vu_arithmetic(pairs,1,vector(6),invalid_uniform); });
    rejects([&] { (void)nt::render::lift_vu_arithmetic({},1,vector(6)); });
    auto invalid=kernel; invalid.inputs.pop_back();
    rejects([&] { nt::render::validate(invalid); });
    invalid=kernel; invalid.arithmetic.expressions[0].left=99999;
    rejects([&] { nt::render::validate(invalid); });
    invalid=kernel; invalid.inputs[0].rate=static_cast<nt::render::InputRate>(99);
    rejects([&] { nt::render::validate(invalid); });
    std::ostringstream cpu,gpu,manifest;
    nt::render::emit_cpu_llvm(cpu,kernel,nt::render::NumericPolicy::native_float32);
    nt::render::emit_spirv_llvm(gpu,kernel,nt::render::NumericPolicy::native_float32);
    nt::render::write_manifest(manifest,kernel);
    require(cpu.str().find("fadd float")!=std::string::npos && cpu.str().find("fmul float")!=std::string::npos,"Native arithmetic not emitted");
    for (const auto* token:{"nt_ee_","nt_gpu_add","%state"," fast "," contract "}) {
        require(cpu.str().find(token)==std::string::npos,"CPU artifact retained machinery or unsafe flags");
        require(gpu.str().find(token)==std::string::npos,"GPU artifact retained machinery or unsafe flags");
    }
    require(gpu.str().find("spirv-unknown-vulkan1.3")!=std::string::npos &&
        gpu.str().find("i32 0, i32 2, i32 1")!=std::string::npos,"GPU uniform binding is absent");
    require(manifest.str().find("\"installable\": false")!=std::string::npos,"Missing installation gate");
    rejects([&] { nt::render::emit_cpu_llvm(cpu,kernel,static_cast<nt::render::NumericPolicy>(99)); });
}
void emit_fixture(const std::string& prefix) {
    const auto kernel=matrix_kernel();
    std::ostringstream cpu,gpu,manifest;
    nt::render::emit_cpu_llvm(cpu,kernel,nt::render::NumericPolicy::native_float32);
    nt::render::emit_spirv_llvm(gpu,kernel,nt::render::NumericPolicy::native_float32);
    nt::render::write_manifest(manifest,kernel);
    nt::write_text_file(prefix+".cpu.ll",cpu.str());
    nt::write_text_file(prefix+".gpu.ll",gpu.str());
    nt::write_text_file(prefix+".json",manifest.str());
    nt::write_file(prefix+".bin",matrix());
}
}
int main(int argc,char** argv) {
    try {
        if (argc==3 && std::string_view(argv[1])=="--emit") { emit_fixture(argv[2]); return 0; }
        if (argc!=1) throw std::invalid_argument("Unexpected test argument");
        run(); std::cout<<"Render lifting, binding, rejection and emission checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
