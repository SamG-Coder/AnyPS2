#pragma once
#include <nt/ee_packet_setup.hpp>
#include <nt/binary.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

namespace probe {
// Opt-in, bounded observation of accepted vector submissions for render-structure
// analysis. It records formats, destinations, sizes, content fingerprints and
// EE source offsets, never executable state, and retains only the latest frames.
// Frames are delimited by displayed-buffer changes.
class VectorCapture {
public:
    VectorCapture(std::filesystem::path directory,std::span<const std::uint8_t> ram,const std::uint64_t* frame)
        : directory_(std::move(directory)),ram_(ram),frame_(frame) {
        if (!std::filesystem::create_directory(directory_))
            throw std::runtime_error("Vector capture directory already exists");
    }
    static void observe(void* raw,unsigned unit,std::span<const nt::ee::NativeSubmissionStep> steps) {
        static_cast<VectorCapture*>(raw)->record(unit,steps);
    }
    void save() const {
        std::ofstream output(directory_/"submissions.tsv");
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output<<"frame\tunit\tstep\tkind\tdestination\tformat\tcomponents\tunsigned\tmode\tcount\tbytes\tfingerprint\tsource\textra\n";
        for (const auto& frame:frames_) output<<frame.text;
    }
private:
    struct Frame { std::uint64_t id=0; std::string text; std::size_t lines=0; };
    // Offsets into EE RAM identify reused source data; other views report -1.
    long long source_offset(std::span<const std::uint8_t> bytes) const {
        if (bytes.empty() || bytes.data()<ram_.data() || bytes.data()>=ram_.data()+ram_.size()) return -1;
        return static_cast<long long>(bytes.data()-ram_.data());
    }
    static std::uint64_t hash(std::span<const std::uint8_t> bytes) { return nt::fingerprint({bytes.data(),bytes.size()}); }
    void record(unsigned unit,std::span<const nt::ee::NativeSubmissionStep> steps) {
        const auto id=frame_ ? *frame_ : 0;
        if (frames_.empty() || frames_.back().id!=id) {
            frames_.push_back({id,{},0});
            if (frames_.size()>retained_frames) frames_.pop_front();
        }
        auto& frame=frames_.back();
        if (frame.lines>=maximum_lines) return;
        std::string& out=frame.text;
        const auto prefix=std::to_string(id)+'\t'+std::to_string(unit)+'\t'+std::to_string(steps_++)+'\t';
        const auto line=[&](const std::string& fields) { out+=prefix+fields+'\n'; ++frame.lines; };
        for (const auto& step:steps) {
            for (const auto& upload:step.uploads)
                line("upload\t"+std::to_string(upload.destination)+"\t\t\t\t\t\t"+std::to_string(upload.bytes.size())+'\t'+
                    std::to_string(hash(upload.bytes))+'\t'+std::to_string(source_offset(upload.bytes))+'\t');
            for (const auto& data:step.data)
                line("data\t"+std::to_string(data.destination)+'\t'+std::to_string(static_cast<unsigned>(data.format))+'\t'+
                    std::to_string(data.components)+'\t'+std::to_string(data.unsigned_values)+'\t'+std::to_string(data.mode)+'\t'+
                    std::to_string(data.output_count)+'\t'+std::to_string(data.bytes.size())+'\t'+std::to_string(hash(data.bytes))+'\t'+
                    std::to_string(source_offset(data.bytes))+'\t'+(data.mask ? "masked" : ""));
            for (const auto& packet:step.drawing)
                line("draw\t\t\t\t\t\t\t"+std::to_string(packet.size()*16)+'\t'+
                    std::to_string(hash({reinterpret_cast<const std::uint8_t*>(packet.data()),packet.size()*16}))+"\t-1\t");
            if (step.launch)
                line("launch\t"+std::to_string(step.launch->entry)+"\t\t\t\t\t\t\t\t\t"+"top="+std::to_string(step.launch->top)+
                    " itop="+std::to_string(step.launch->itop)+(step.launch->continuation ? " continue" : ""));
        }
    }
    static constexpr std::size_t retained_frames=16,maximum_lines=200000;
    std::filesystem::path directory_;
    std::span<const std::uint8_t> ram_;
    const std::uint64_t* frame_;
    std::deque<Frame> frames_;
    std::uint64_t steps_=0;
};
}
