#include <nt/iso9660.hpp>
#include <nt/elf.hpp>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: ps2-import <disc.iso> <new-output-directory>\n";
        return 2;
    }
    try {
        nt::disc::Iso9660 disc(argv[1]);
        const auto cnf = disc.read(disc.find("SYSTEM.CNF"), 65536);
        const auto boot = nt::disc::boot_path(cnf);
        const auto bytes = disc.read(disc.find(boot));
        const auto elf = nt::elf::parse(bytes);
        std::ostringstream report;
        report << "AnyPS2 import v1\nVolume: " << disc.volume() << "\nBoot: " << boot << '\n';
        nt::elf::describe(report, elf);
        report << "Root directory:\n";
        for (const auto& entry : disc.entries(disc.root()))
            report << "  " << entry.name << " bytes=" << entry.size << " sector=" << entry.sector << '\n';
        const std::filesystem::path output(argv[2]);
        if (!std::filesystem::create_directories(output))
            throw std::runtime_error("Output directory already exists; refusing to overwrite");
        nt::write_file(output / "boot.elf", bytes);
        nt::write_file(output / "SYSTEM.CNF", cnf);
        const auto text = report.str();
        nt::write_file(output / "report.txt", nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        std::cout << text;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ps2-import: " << error.what() << '\n';
        return 1;
    }
}
