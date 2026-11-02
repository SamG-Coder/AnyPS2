#pragma once
#include <nt/disc_files.hpp>
#include <nt/runtime.hpp>
#include <functional>
#include <optional>

namespace nt::fileio {
struct Write { std::uint32_t address; Bytes bytes; };
struct Reply { Bytes result; std::vector<Write> writes; };
struct Statistics { std::uint64_t requests = 0, opens = 0, bytes_read = 0; std::uint32_t last_method = 0; };
using ConsoleWriter=std::function<std::int32_t(std::uint32_t,ByteView)>;
// Read-only disc files and explicitly bound host output streams. Guest paths
// never name host files; an empty image leaves the disc namespace unmounted.
class Service {
public:
    explicit Service(const std::filesystem::path& image,ConsoleWriter output={}) : output_(std::move(output)) {
        if (!image.empty()) files_.emplace(image);
    }
    Reply call(std::uint32_t method, ByteView arguments, std::uint32_t result_size,ByteView source={});
    Statistics statistics() const { return statistics_; }
    void reset() { if (files_) files_->reset(); }
    Bytes read_disc_file(const std::string& guest_path, std::uint64_t limit);
private:
    std::optional<disc::Files> files_;
    ConsoleWriter output_;
    Statistics statistics_;
};
}
