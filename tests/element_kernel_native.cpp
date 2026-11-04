#include <nt/ee_float.hpp>
#include <nt/loop_pipeline.hpp>
#include <nt/vu_quotient.hpp>
#if defined(ANYPS2_ELEMENT_VULKAN)
#include "vulkan_compute.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Executes the LLVM-compiled element kernel of the synthetic quotient loop from
// loop_structure.cpp (out = in * (in.x / sqrt(in.y)) per element) and, when given a
// SPIR-V path, the same kernel on a Vulkan device. Expected values come from the
// exact VU arithmetic definitions, not from the emitted graph.
extern "C" void nt_render_elements(const std::uint32_t*,const std::uint32_t*,std::uint32_t*);
namespace {
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
constexpr unsigned rows=1024,words_per_element=5,input_base=1000,first=5,count=200;
constexpr std::uint32_t canary=0x7fcabcdeU;

std::vector<std::uint32_t> memory() {
    std::mt19937 random(0x0e1e);
    std::vector<std::uint32_t> data(rows*4);
    for (auto& bits:data) bits=(random()&0x807fffffU)|((100U+random()%50U)<<23);
    const auto row=[&](unsigned element) { return ((input_base+first+element)&(rows-1))*4; };
    data[row(30)+1]=0x80000000U;               // reciprocal of signed zero
    data[row(35)+1]^=0x80000000U;              // negative radicand: invalid flag, finite result
    data[row(31)+0]=0x7f800000U;               // exponent 255 numerator: VU stop
    data[row(32)+1]=0x7f812345U;               // exponent 255 denominator: VU stop
    data[row(33)+0]=0x7f000000U; data[row(33)+1]=0x00800001U;  // quotient overflow
    data[row(34)+2]=0x7f000000U;               // product beyond the finite range
    return data;
}
std::vector<std::uint32_t> expected(const std::vector<std::uint32_t>& data) {
    std::vector<std::uint32_t> out(count*words_per_element);
    for (unsigned element=0;element<count;++element) {
        const auto* in=&data[((input_base+first+element)&(rows-1))*4];
        const auto quotient=nt::numeric::vu_reciprocal_square_root_finite(in[0],in[1]);
        auto* result=&out[element*words_per_element];
        result[0]=quotient.supported ? 0U : 1U;
        for (unsigned lane=0;lane<4;++lane) {
            const auto product=nt::numeric::multiply_finite(in[lane],quotient.bits,true);
            result[0]|=product.supported ? 0U : 1U;
            result[1+lane]=product.bits;
        }
    }
    return out;
}
void check_elements(const std::vector<std::uint32_t>& actual,const std::vector<std::uint32_t>& reference,const char* where) {
    for (unsigned n=0;n<reference.size();++n)
        require(actual[n]==reference[n],std::string(where)+" element word "+std::to_string(n)+" differs");
    require(std::all_of(actual.begin()+reference.size(),actual.end(),[](auto x) { return x==canary; }),
        std::string(where)+" wrote past the element range");
}
// The shader conversions use portable integer definitions; the host uses
// exact double-based forms. Both must agree for every input class.
void check_portable_conversions() {
    std::mt19937 random(0x5eed);
    for (unsigned n=0;n<(1U<<22);++n) {
        const std::uint32_t bits=n<4096 ? (n<<20)|(n&1 ? 0x7ffffU : 0U) : static_cast<std::uint32_t>(random());
        require(nt::numeric::integer_to_scalar(bits)==nt::numeric::integer_to_scalar_portable(bits),"Integer conversion forms differ");
        require(nt::numeric::scalar_to_integer(bits)==nt::numeric::scalar_to_integer_portable(bits),"Scalar conversion forms differ");
        const auto radicand=(static_cast<std::uint64_t>(random())<<16|random()>>16)&((1ULL<<48)-1);
        const auto root=nt::numeric::integer_square_root_portable(radicand);
        require(root*root<=radicand && (root+1)*(root+1)>radicand,"Portable integer square root is not the floor root");
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc<=2,"Usage: element-kernel-native-tests [compiled SPIR-V]");
        check_portable_conversions();
        nt::render::ElementEntry entry;
        entry.integers[3]=input_base; entry.integers[4]=500;
        const auto data=memory(),original=data;
        const auto parameters=nt::render::element_parameters(entry,first,count);
        const auto reference=expected(data);
        require(reference[30*words_per_element]==0 && reference[31*words_per_element]==1 &&
            reference[32*words_per_element]==1,"Fixture does not cover quotient flags and VU stops");
        std::vector<std::uint32_t> output(reference.size()+64,canary);
        nt_render_elements(data.data(),parameters.data(),output.data());
        require(data==original,"CPU element kernel modified its source");
        check_elements(output,reference,"CPU");
        auto empty=parameters; empty[0]=0; std::fill(output.begin(),output.end(),canary);
        nt_render_elements(data.data(),empty.data(),output.data());
        require(std::all_of(output.begin(),output.end(),[](auto x) { return x==canary; }),"Empty range wrote output");
        std::cout<<"CPU: "<<count<<" compiled element kernels match exact VU arithmetic\n";
        if (argc==2) {
#if defined(ANYPS2_ELEMENT_VULKAN)
            const auto shader=vulkan_compute::read_shader(argv[1]);
            auto gpu_data=data,gpu_parameters=parameters;
            const unsigned groups=(count+63)/64;
            std::vector<std::uint32_t> gpu_output(groups*64*words_per_element,canary);
            vulkan_compute::Device device; device.initialize();
            const std::span<std::uint32_t> buffers[]{gpu_data,gpu_output,gpu_parameters};
            device.run(shader,buffers,groups);
            check_elements(gpu_output,reference,"Vulkan");
            std::cout<<"Vulkan: "<<count<<" element kernels match exact VU arithmetic\n";
#else
            throw std::invalid_argument("This build has no Vulkan element runner");
#endif
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
