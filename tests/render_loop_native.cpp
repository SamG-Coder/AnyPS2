#include "render_loop_fixture.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <random>
#include <vector>

extern "C" void nt_render_loop(const std::uint32_t*,const std::uint32_t*,std::uint32_t*);
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void run() {
    constexpr unsigned variant=ANYPS2_LOOP_VARIANT;
    const auto c=render_fixture::configuration(variant);
    const auto loop=render_fixture::loop(variant);
    std::mt19937 random(0x51a700+variant);
    constexpr std::uint32_t canary=0x7fcabcdeU;
    std::vector<std::uint32_t> source(4096),output(4096,canary);
    unsigned tested=0;
    for (unsigned test=0;test<128;++test) {
        const unsigned count=1+(test*17)%128;
        for (auto& bits:source) {
            // Bounded finite values exercise signs, mantissas and moderate exponents.
            bits=(random()&0x807fffffU)|((120U+random()%15U)<<23);
        }
        const auto original=source;
        const auto bindings=nt::render::bind_vertex_loop(loop,render_fixture::integers(variant,count),source);
        std::fill(output.begin(),output.end(),canary);
        nt_render_loop(source.data(),bindings.parameters.data(),output.data()+4);
        require(source==original,"Compiled loop modified its source");
        for (unsigned item=0;item<count;++item) {
            const unsigned vertex_row=16+item*c.input_stride,uniform_row=variant ? 8 : 0;
            for (unsigned lane=0;lane<4;++lane) {
                const auto component=[&](unsigned row,unsigned component) { return std::bit_cast<float>(source[row*4+component]); };
                // Independent modern float32 reference, not an interpreter of the emitted graph.
                float expected=component(uniform_row,lane)*component(vertex_row,0);
                expected=expected+component(uniform_row+1,lane)*component(vertex_row,1);
                expected=expected+component(uniform_row+2,lane)*component(vertex_row,2);
                expected=expected+component(uniform_row+3,lane)*component(vertex_row,3);
                require(output[4+item*4+lane]==std::bit_cast<std::uint32_t>(expected),"Compiled strided transform differs from reference");
            }
        }
        require(std::all_of(output.begin(),output.begin()+4,[&](auto x) { return x==canary; }),"Output underflow");
        require(std::all_of(output.begin()+4+count*4,output.end(),[&](auto x) { return x==canary; }),"Output overflow");
        tested+=count;
    }
    auto bindings=nt::render::bind_vertex_loop(loop,render_fixture::integers(variant,1),source);
    for (auto count:{0U,1025U,0xffffffffU}) {
        bindings.parameters[0]=count; std::fill(output.begin(),output.end(),canary);
        nt_render_loop(source.data(),bindings.parameters.data(),output.data());
        require(std::all_of(output.begin(),output.end(),[&](auto x) { return x==canary; }),"Rejected count wrote output");
    }
    std::cout<<"Variant "<<variant<<": "<<tested<<" compiled strided transforms and bounds checks passed\n";
}
}
int main() {
    try { run(); return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
