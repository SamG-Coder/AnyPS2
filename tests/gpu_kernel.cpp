#include "vulkan_compute.hpp"
#include <nt/gpu_ir.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void nt_gpu_reference(const std::uint32_t*,std::uint32_t*,std::uint32_t);
using vulkan_compute::check;
int main(int argc,char** argv) {
    try {
        check(argc==2,"Expected compiled shader path");
        const auto shader=vulkan_compute::read_shader(argv[1]);
        constexpr unsigned items=1025,input_stride=20,output_stride=7;
        std::vector<std::uint32_t> input(1+items*input_stride),expected((items+63)*output_stride,0xdeadbeefU),actual=expected;
        input[0]=items; std::uint32_t random=0x346ae517;
        for (unsigned n=1;n<input.size();++n) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            input[n]=n%5 ? random : (n%10 ? 0x80000000U : 0U);
        }
        for (unsigned n=0;n<items+63;++n) nt_gpu_reference(input.data(),expected.data(),n);
        vulkan_compute::Device device; device.initialize();
        const std::span<std::uint32_t> buffers[]{input,actual};
        device.run(shader,buffers,(input[0]+63)/64);
        for (unsigned n=0;n<actual.size();++n) if (actual[n]!=expected[n])
            throw std::runtime_error("GPU arithmetic differs at word "+std::to_string(n)+
                ": expected "+std::to_string(expected[n])+", got "+std::to_string(actual[n]));
        nt::gpu::Program invalid; invalid.inputs=1;
        invalid.expressions.push_back({nt::gpu::Operation::add,0,1}); invalid.outputs.push_back(1);
        bool rejected=false; try { nt::gpu::validate(invalid); } catch (const std::invalid_argument&) {rejected=true;}
        check(rejected,"Forward dependency was accepted");
        std::cout<<"Verified "<<items<<" AOT matrix batches, exact values/flags and inactive-lane sentinels.\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
