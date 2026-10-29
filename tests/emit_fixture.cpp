#include "native_fixture.hpp"
#include <nt/codegen.hpp>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    nt::analysis::Program program;
    for (const auto& [pc, word] : fixture::words()) program.instructions.emplace(pc, nt::r5900::decode(pc, word));
    std::ofstream out(argv[1]);
    nt::codegen::emit(out, program);
    out.close();
    if (!out) { std::cerr << "Cannot write generated test fixture\n"; return 1; }
    return 0;
}
