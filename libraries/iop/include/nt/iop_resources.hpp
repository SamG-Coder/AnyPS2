#pragma once
#include <nt/binary.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nt::iop {
struct ResourceFile { std::string name; Bytes data; };
// Original read-only ROMDIR-format file container, not executable firmware.
// Only explicitly supplied resources exist. The default directory has no files.
class Resources {
public:
    explicit Resources(std::span<const ResourceFile> files={});
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes) const;
    ByteView image() const { return image_; }
private:
    Bytes image_;
};
}
