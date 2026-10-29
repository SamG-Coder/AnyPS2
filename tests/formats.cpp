#include "disc_fixture.hpp"
#include <nt/elf.hpp>
#include <nt/iso9660.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using fixture::put32; using fixture::both32; using fixture::record;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Malformed input was accepted");
}
nt::Bytes make_elf() {
    nt::Bytes b(260, 0);
    const std::uint8_t magic[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::copy(std::begin(magic), std::end(magic), b.begin());
    b[16] = 2; b[18] = 8; b[20] = 1; b[40] = 52; b[42] = 32; b[44] = 1;
    put32(b, 24, 0x100000); put32(b, 28, 52);
    put32(b, 52, 1); put32(b, 56, 256); put32(b, 60, 0x100000);
    put32(b, 68, 4); put32(b, 72, 8); put32(b, 76, 5); put32(b, 80, 4);
    return b;
}
void test_elf() {
    const auto good = make_elf();
    const auto parsed = nt::elf::parse(good);
    check(parsed.entry == 0x100000 && parsed.segments.size() == 1, "ELF metadata differs");
    for (std::size_t length = 0; length < good.size(); ++length)
        rejects([&] { nt::elf::parse(nt::ByteView(good).first(length)); });
    for (const auto offset : {18U, 42U, 44U}) {
        auto bad = good; bad[offset] = 0;
        rejects([&] { nt::elf::parse(bad); });
    }
    auto bad = good; put32(bad, 24, 0x100004);
    rejects([&] { nt::elf::parse(bad); });
    bad = good; put32(bad, 72, 3);
    rejects([&] { nt::elf::parse(bad); });
    bad = good; put32(bad, 28, 0xfffffff0);
    rejects([&] { nt::elf::parse(bad); });
}
void test_disc(const std::filesystem::path& path) {
    nt::Bytes iso(24 * 2048, 0);
    const auto pvd = 16 * 2048;
    iso[pvd] = 1;
    const std::string signature = "CD001";
    std::copy(signature.begin(), signature.end(), iso.begin() + pvd + 1);
    iso[pvd + 6] = 1; iso[pvd + 129] = 8; iso[pvd + 130] = 8;
    record(iso, pvd + 156, std::string(1, '\0'), 20, 2048, true);
    const std::string cnf = "BOOT2 = cdrom0:\\TEST.ELF;1\r\n";
    auto at = 20 * 2048;
    at += static_cast<int>(record(iso, at, "SYSTEM.CNF;1", 21, static_cast<std::uint32_t>(cnf.size()), false));
    const auto elf = make_elf();
    record(iso, at, "TEST.ELF;1", 22, static_cast<std::uint32_t>(elf.size()), false);
    std::copy(cnf.begin(), cnf.end(), iso.begin() + 21 * 2048);
    std::copy(elf.begin(), elf.end(), iso.begin() + 22 * 2048);
    nt::write_file(path, iso);
    {
        nt::disc::Iso9660 disc(path);
        const auto boot = nt::disc::boot_path(disc.read(disc.find("system.cnf")));
        check(boot == "TEST.ELF;1", "BOOT2 parsing failed");
        check(disc.read(disc.find(boot)) == elf, "ISO extraction changed bytes");
        rejects([&] { disc.find("../TEST.ELF"); });
        rejects([&] { disc.find("MISSING.ELF"); });
        rejects([&] { disc.read({"outside", 1000, 4, false}); });
    }
    iso[pvd + 156 + 6] ^= 1;
    nt::write_file(path, iso);
    rejects([&] { nt::disc::Iso9660 disc(path); });
    nt::Bytes empty;
    rejects([&] { nt::disc::boot_path(empty); });
}
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("anyps2-format-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".iso");
    try {
        test_elf();
        test_disc(path);
        std::filesystem::remove(path);
        std::cout << "ELF and ISO positive/negative cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
