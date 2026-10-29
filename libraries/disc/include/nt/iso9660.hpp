#pragma once
#include <nt/binary.hpp>
#include <fstream>

namespace nt::disc {
struct Entry {
    std::string name;
    std::uint32_t sector, size;
    bool directory;
};
class Iso9660 {
public:
    explicit Iso9660(const std::filesystem::path& path);
    const std::string& volume() const { return volume_; }
    std::vector<Entry> entries(const Entry& directory);
    const Entry& root() const { return root_; }
    Entry find(std::string path);
    Bytes read(const Entry& entry, std::uint64_t limit = 128 * 1024 * 1024);
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
