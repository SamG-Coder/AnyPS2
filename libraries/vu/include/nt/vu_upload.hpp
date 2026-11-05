#pragma once
#include <nt/vu_program.hpp>
#include <nt/elf.hpp>

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
// Finds programs stored as ready-to-send uploads when a program declares none.
// Evidence is structural: a qword-aligned DMA tag whose payload follows it and
// whose size exactly frames VIF uploads, assembling into an image that contains
// an end marker. Candidates are compilation inputs; uploads still validate at runtime.
std::vector<Upload> find_framed_uploads(ByteView bytes);
struct DeclaredUploadImage {
    Upload image;
    std::vector<std::uint32_t> sources;
};
// Only adjacent destination ranges joined by proven NOP/MPG framing are
// coalesced. A launch, descriptor, interruption or destination reset separates
// artifacts. No instruction decoding or execution is used for recovery.
std::vector<DeclaredUploadImage> recover_declared_uploads(ByteView file,std::span<const elf::VectorOverlay> overlays);
}
