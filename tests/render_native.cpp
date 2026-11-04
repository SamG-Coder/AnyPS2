#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

extern "C" void nt_render_reference(const std::uint32_t*,const std::uint32_t*,float*,std::uint32_t);
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
float reference(const std::array<float,16>& matrix,const std::array<float,4>& vertex,unsigned lane) {
    volatile float product=matrix[lane]*vertex[0];
    volatile float sum=product;
    for (unsigned column=1;column<4;++column) {
        product=matrix[column*4+lane]*vertex[column];
        sum=sum+product;
    }
    return sum;
}
void run() {
    constexpr unsigned count=4097;
    constexpr float sentinel=-12345.0f;
    std::vector<std::uint32_t> input(1+count*4); input[0]=count;
    std::vector<std::array<float,4>> vertices(count);
    std::uint32_t random=17;
    for (auto& vertex:vertices) {
        for (auto& lane:vertex) {
            random=random*1664525U+1013904223U;
            // Finite, varied mantissas/exponents exercise rounding and cancellation.
            lane=std::bit_cast<float>((random&0x807fffffU)|((118U+((random>>24)&15U))<<23));
        }
    }
    for (unsigned i=0;i<count;++i) for (unsigned lane=0;lane<4;++lane)
        input[1+i*4+lane]=std::bit_cast<std::uint32_t>(vertices[i][lane]);
    for (unsigned pass=0;pass<2;++pass) {
        std::array<float,16> matrix{}; std::array<std::uint32_t,16> uniforms{};
        for (unsigned n=0;n<16;++n) {
            random=random*1664525U+1013904223U;
            matrix[n]=std::bit_cast<float>((random&0x807fffffU)|((119U+((random>>24)&15U))<<23));
            uniforms[n]=std::bit_cast<std::uint32_t>(matrix[n]);
        }
        std::vector<float> output(count*4+4,sentinel);
        for (unsigned i=0;i<count+3;++i) nt_render_reference(input.data(),uniforms.data(),output.data(),i);
        for (unsigned i=0;i<count;++i) for (unsigned lane=0;lane<4;++lane) {
            const auto expected=reference(matrix,vertices[i],lane);
            require(std::bit_cast<std::uint32_t>(output[i*4+lane])==std::bit_cast<std::uint32_t>(expected),
                "Compiled LLVM transform differs from the independent native-float32 reference");
        }
        for (unsigned n=count*4;n<output.size();++n) require(output[n]==sentinel,"Inactive invocation wrote output");
        nt_render_reference(input.data(),nullptr,nullptr,0xffffffffU);
    }
    // Out-of-contract counts must exit before fetching uniforms or writing output.
    const std::array<std::uint32_t,1> empty{0},oversized{65537};
    nt_render_reference(empty.data(),nullptr,nullptr,0);
    nt_render_reference(oversized.data(),nullptr,nullptr,0);
}
}
int main() {
    try { run(); std::cout<<"Compiled LLVM: 8194 transforms matched; uniform rebinding and bounds passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
