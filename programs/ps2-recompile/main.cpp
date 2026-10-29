#include <nt/ee_codegen.hpp>
#include <iostream>
int main(int argc,char** argv) {
    if (argc!=3) {
        std::cerr<<"Usage: ps2-recompile <boot.elf> <new-output-directory>\n"
                 <<"LLVM entry-flow conversion follows supported native syscall continuations.\n"; return 2;
    }
    try {
        nt::ee_codegen::project(argv[1],argv[2]);
        std::cout<<"Generated EE LLVM project. Compile it with ps2-build. Game boot is not established.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"EE conversion: "<<error.what()<<'\n'; return 1; }
}
