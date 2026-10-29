#include <nt/codegen.hpp>
#include <iostream>
#include <charconv>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: ps2-recompile <boot.elf> <new-output-directory> [--entry=0xADDRESS ...] [--capture=DIRECTORY]\n";
        return 2;
    }
    try {
        std::vector<std::uint32_t> entries;
        std::filesystem::path capture;
        for (int index = 3; index < argc; ++index) {
            std::string_view value(argv[index]);
            if (value.starts_with("--capture=")) {
                capture = std::string(value.substr(10));
                if (capture.empty()) throw std::runtime_error("Capture path is empty");
                continue;
            }
            if (!value.starts_with("--entry=0x")) throw std::runtime_error("Expected --entry=0xADDRESS");
            value.remove_prefix(10);
            std::uint32_t address = 0;
            const auto parsed = std::from_chars(value.data(),value.data()+value.size(),address,16);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size()) throw std::runtime_error("Invalid entry address");
            entries.push_back(address);
        }
        nt::codegen::project(argv[1], argv[2], entries, capture);
        std::cout << "Generated diagnostic native project: " << argv[2]
                  << "\nKernel and hardware boundaries stop execution; this is not a playable game build.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ps2-recompile: " << error.what() << '\n';
        return 1;
    }
}
