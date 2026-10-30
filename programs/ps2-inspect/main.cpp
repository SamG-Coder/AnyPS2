#include <nt/elf.hpp>
#include <nt/analysis.hpp>
#include <nt/metrics.hpp>
#include <iostream>
#include <charconv>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4 || (argc >= 3 && std::string_view(argv[2]) != "--flow" && std::string_view(argv[2]) != "--metrics") ||
        (argc == 4 && std::string_view(argv[2]) != "--metrics")) {
        std::cerr << "Usage: ps2-inspect <executable.elf> [--flow|--metrics [window]]\n";
        return 2;
    }
    try {
        std::size_t window=128;
        if (argc==4) {
            const std::string_view value(argv[3]);
            const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),window);
            if (error!=std::errc{} || end!=value.data()+value.size() || !window || window>4096)
                throw std::invalid_argument("Metric analysis window must be an integer in 1..4096");
        }
        const auto bytes = nt::read_file(argv[1]);
        const auto elf = nt::elf::parse(bytes);
        nt::elf::describe(std::cout, elf);
        if (argc >= 3 && std::string_view(argv[2]) == "--metrics")
            nt::analysis::describe_metrics(std::cout,nt::analysis::inspect_metrics(bytes,elf,window));
        else if (argc == 3) nt::analysis::describe(std::cout, nt::analysis::discover(bytes, elf));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ps2-inspect: " << error.what() << '\n';
        return 1;
    }
}
