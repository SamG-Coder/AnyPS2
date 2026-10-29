#pragma once
#include <nt/binary.hpp>
#include <fstream>
#include <array>

namespace nt::disc {
class PathError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct Entry {
    std::string name;
    std::uint32_t sector, size;
    bool directory;
    std::array<std::uint8_t,7> recorded{};
};
class Iso9660 {
public:
    explicit Iso9660(const std::filesystem::path& path);
    const std::string& volume() const { return volume_; }
    std::vector<Entry> entries(const Entry& directory);
    const Entry& root() const { return root_; }
    Entry find(std::string path);
    Bytes read(const Entry& entry, std::uint64_t limit = 128 * 1024 * 1024);
    Bytes read_range(const Entry& entry, std::uint32_t offset, std::uint32_t count);
private:
    Bytes read_at(std::uint64_t offset, std::uint64_t size);
    static Entry record(ByteView bytes);
    std::ifstream file_;
    std::uint64_t size_;
    Entry root_{};
    std::string volume_;
};
std::string boot_path(ByteView system_cnf);
}
