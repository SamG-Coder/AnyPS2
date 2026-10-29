#include <nt/elf.hpp>
#include <nt/analysis.hpp>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--flow")) {
        std::cerr << "Usage: ps2-inspect <executable.elf> [--flow]\n";
        return 2;
    }
    try {
        const auto bytes = nt::read_file(argv[1]);
        const auto elf = nt::elf::parse(bytes);
        nt::elf::describe(std::cout, elf);
        if (argc == 3) nt::analysis::describe(std::cout, nt::analysis::discover(bytes, elf));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ps2-inspect: " << error.what() << '\n';
        return 1;
    }
}
