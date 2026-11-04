#pragma once
#include "capture.hpp"
#include <nt/graphics_packet.hpp>
#include <nt/native_scanout.hpp>
#include <chrono>

namespace probe {
// Opt-in, bounded observation of submitted graphics data. This is never read by
// the game runtime, and must stay beside other private conversion artifacts.
class GraphicsCapture {
public:
    explicit GraphicsCapture(const std::filesystem::path& directory,bool display_only=false)
        : directory_(directory),display_only_(display_only) {
        if (!std::filesystem::create_directory(directory_))
            throw std::runtime_error("Graphics capture directory already exists");
        packets_.exceptions(std::ios::failbit|std::ios::badbit);
        frames_.exceptions(std::ios::failbit|std::ios::badbit);
        if (!display_only_) packets_.open(directory_/"packets.bin",std::ios::binary);
        frames_.open(directory_/"frames.tsv");
        if (!display_only_) word(1); // Version; subsequent records are little-endian uint32 words.
        frames_<<"frame\tmilliseconds\tbatch\tdraws\tcontrol\tbuffer0\tbuffer1\tdisplay0\tdisplay1\thalf_height\tfield\n";
    }
    void reset() { if (!display_only_) { reserve(4); word(2); } }
    bool ready() const {
        return frames_count_<(display_only_ ? 360U : 128U) &&
            image_bytes_<1024ULL*1024*1024 && std::chrono::steady_clock::now()>=next_frame_;
    }
    void submit(std::span<const nt::graphics::Packet> packets) {
        if (display_only_) { ++batches_; return; }
        std::size_t bytes=8+packets.size()*4;
        for (const auto& packet:packets) bytes+=packet.size()*16;
        if (batches_==65536)
            throw std::runtime_error("Graphics capture reached its diagnostic limit");
        reserve(bytes);
        word(1); word(static_cast<std::uint32_t>(packets.size()));
        for (const auto& packet:packets) {
            word(static_cast<std::uint32_t>(packet.size()));
            packets_.write(reinterpret_cast<const char*>(packet.data()),packet.size()*16);
        }
        ++batches_;
    }
    void present(const nt::graphics::Scanout& frame,std::uint64_t draws,std::uint64_t control,
                 const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,bool half_height,bool field) {
        const auto now=std::chrono::steady_clock::now();
        if (!ready()) return;
        const auto frame_bytes=frame.pixels.size()*3+64;
        if (frame_bytes>1024ULL*1024*1024-image_bytes_) return;
        // Short consecutive bursts expose field alternation without making
        // readback/PPM I/O dominate every display deadline during the run.
        if (display_only_) {
            if (++burst_frames_==6) { burst_frames_=0; next_frame_=now+std::chrono::seconds(10); }
        } else next_frame_=now+std::chrono::milliseconds(500);
        const auto hash=nt::fingerprint({reinterpret_cast<const std::uint8_t*>(frame.pixels.data()),frame.pixels.size()*4});
        if (frames_count_ && hash==last_hash_ && half_height==last_half_height_) return;
        last_hash_=hash; last_half_height_=half_height;
        capture_image(directory_/("frame-"+std::to_string(frames_count_)+".ppm"),frame.width,frame.height,
            [&](unsigned x,unsigned y) { return frame.pixels[y*frame.width+x]; });
        image_bytes_+=frame_bytes;
        frames_<<frames_count_++<<'\t'<<std::chrono::duration_cast<std::chrono::milliseconds>(now-started_).count()
               <<'\t'<<batches_<<'\t'<<draws<<'\t'<<control<<'\t'<<buffers[0]<<'\t'<<buffers[1]
               <<'\t'<<displays[0]<<'\t'<<displays[1]<<'\t'<<half_height<<'\t'<<field<<'\n';
        frames_.flush(); if (!display_only_) packets_.flush();
    }
private:
    void reserve(std::size_t bytes) {
        if (bytes>128*1024*1024-bytes_) throw std::runtime_error("Graphics capture reached its diagnostic limit");
        bytes_+=bytes;
    }
    void word(std::uint32_t value) {
        const char bytes[]{char(value),char(value>>8),char(value>>16),char(value>>24)};
        packets_.write(bytes,4);
    }
    std::filesystem::path directory_;
    std::ofstream packets_,frames_;
    std::chrono::steady_clock::time_point started_=std::chrono::steady_clock::now(),next_frame_{};
    std::size_t bytes_=4;
    std::size_t image_bytes_=0;
    bool display_only_=false;
    std::uint64_t last_hash_=0;
    bool last_half_height_=false;
    unsigned batches_=0,frames_count_=0,burst_frames_=0;
};
}
