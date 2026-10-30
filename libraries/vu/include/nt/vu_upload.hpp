#pragma once
#include <nt/vu_program.hpp>

namespace nt::vu {
struct Upload {
    unsigned destination;
    Bytes bytes;
};
struct UploadSequence {
    std::vector<Upload> uploads;
    std::size_t consumed=0;
    std::optional<unsigned> stopped_command;
};
// Offline framing only. Stops before unknown commands and launches; never scans
// payload words for apparent commands or executes uploaded instructions.
UploadSequence extract_uploads(ByteView packet,unsigned unit);
// Assemble the final image before the extraction boundary. Later uploads replace
// earlier bytes; holes stay separate instead of being filled with invented code.
std::vector<Upload> assemble_uploads(std::span<const Upload> uploads,unsigned unit);
}
