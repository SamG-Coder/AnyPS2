#pragma once
#include <nt/disc_files.hpp>
#include <nt/runtime.hpp>

namespace nt::fileio {
struct Write { std::uint32_t address; Bytes bytes; };
struct Reply { Bytes result; std::vector<Write> writes; };
struct Statistics { std::uint64_t requests = 0, opens = 0, bytes_read = 0; std::uint32_t last_method = 0; };
// Read-only legacy FILEIO RPC service. Guest paths never name host files.
class Service {
public:
    explicit Service(const std::filesystem::path& image) : files_(image) {}
    Reply call(std::uint32_t method, ByteView arguments, std::uint32_t result_size);
    Statistics statistics() const { return statistics_; }
    void reset() { files_.reset(); }
    Bytes read_disc_file(const std::string& guest_path, std::uint64_t limit);
private:
    disc::Files files_;
    Statistics statistics_;
};
}
