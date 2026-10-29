#include <iostream>

int main() {
    std::cerr << "AnyPS2 uses LLVM ahead-of-time compilation only.\n"
        "EE LLVM conversion is not implemented yet. No output was generated.\n"
        "Use ps2-iop-recompile for supported IOP module conversion.\n";
    return 1;
}
