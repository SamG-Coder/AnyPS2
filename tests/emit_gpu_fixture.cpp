#include <nt/gpu_ir.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        if (argc!=3) throw std::invalid_argument("Expected GPU and CPU LLVM output paths");
        // Original fixture: four independent dot products. No game instructions
        // or game addresses are used to create this arithmetic graph.
        nt::gpu::Program program; program.inputs=20;
        const auto append=[&](nt::gpu::Operation op,unsigned a,unsigned b) {
            const auto id=program.inputs+static_cast<unsigned>(program.expressions.size());
            program.expressions.push_back({op,a,b}); return id;
        };
        for (unsigned row=0;row<4;++row) {
            auto sum=append(nt::gpu::Operation::multiply,row*4,16);
            for (unsigned column=1;column<4;++column) {
                const auto product=append(nt::gpu::Operation::multiply,row*4+column,16+column);
                sum=append(nt::gpu::Operation::add,sum,product);
            }
            program.outputs.push_back(sum);
        }
        program.outputs.push_back(append(nt::gpu::Operation::subtract,program.outputs[0],program.outputs[1]));
        const auto constant=program.inputs+static_cast<unsigned>(program.expressions.size());
        program.expressions.push_back({nt::gpu::Operation::constant,0,0,0x80000000U});
        program.outputs.push_back(constant);
        std::ofstream gpu(argv[1]),cpu(argv[2]);
        gpu.exceptions(std::ios::badbit|std::ios::failbit);
        cpu.exceptions(std::ios::badbit|std::ios::failbit);
        nt::gpu::emit_spirv_llvm(gpu,program); nt::gpu::emit_cpu_llvm(cpu,program);
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
