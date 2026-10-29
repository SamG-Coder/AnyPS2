#pragma once
#include <nt/iso9660.hpp>
#include <nt/runtime.hpp>
#include <map>

namespace nt::fileio {
struct Write { std::uint32_t address; Bytes bytes; };
struct Reply { Bytes result; std::vector<Write> writes; };
struct Statistics { std::uint64_t requests = 0, opens = 0, bytes_read = 0; std::uint32_t last_method = 0; };
// Read-only legacy FILEIO RPC service. Guest paths never name host files.
class Service {
public:
    explicit Service(const std::filesystem::path& image) : disc_(image) {}
    Reply call(std::uint32_t method, ByteView arguments, std::uint32_t result_size);
    Statistics statistics() const { return statistics_; }
    void reset() { handles_.clear(); }
    Bytes read_disc_file(const std::string& guest_path, std::uint64_t limit);
private:
    struct Handle { disc::Entry entry; std::uint32_t offset = 0; };
    disc::Iso9660 disc_;
    std::map<std::uint32_t,Handle> handles_;
    Statistics statistics_;
};
}
