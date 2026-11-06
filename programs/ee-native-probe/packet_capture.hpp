#pragma once
#include <nt/binary.hpp>
#include <nt/ee_platform.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace probe {
// Opt-in private record of accepted VIF and GIF transfers, so that the path from a
// transfer to a queued VU job can run by itself without the game or a window. Program
// uploads are always kept, because a replay needs the programs; other transfers are
// kept only once armed, up to a byte limit.
class PacketCapture {
public:
    explicit PacketCapture(const std::filesystem::path& path) : file_(path,std::ios::binary|std::ios::trunc) {
        if (!file_) throw std::runtime_error("Cannot create packet capture");
    }
    void arm() { armed_=true; }
    static void observe_submission(void* raw,unsigned,std::span<const nt::ee::NativeSubmissionStep> steps) {
        auto& self=*static_cast<PacketCapture*>(raw);
        for (const auto& step:steps) if (!step.uploads.empty()) self.uploads_=true;
    }
    static void observe_packet(void* raw,unsigned channel,std::span<const std::uint8_t> bytes,std::span<const nt::ee::NativePacketOrigin> origins) {
        auto& self=*static_cast<PacketCapture*>(raw);
        const bool uploads=self.uploads_; self.uploads_=false;
        if ((!self.armed_ && !uploads) || self.written_>maximum_bytes) return;
        const std::uint32_t header[3]{channel,std::uint32_t(bytes.size()),std::uint32_t(origins.size())};
        self.file_.write(reinterpret_cast<const char*>(header),sizeof(header));
        self.file_.write(reinterpret_cast<const char*>(origins.data()),std::streamsize(origins.size_bytes()));
        self.file_.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        self.written_+=sizeof(header)+origins.size_bytes()+bytes.size();
    }
private:
    static constexpr std::uint64_t maximum_bytes=std::uint64_t(1500)*1024*1024;
    std::ofstream file_;
    std::uint64_t written_=0;
    bool armed_=false,uploads_=false;
};

// Receives what the replay would have drawn. Equal hashes between builds show that a
// change to the path left its output exact.
struct PacketReplaySink {
    std::uint64_t hash=0xcbf29ce484222325ULL,packets=0,quadwords=0,hash_ns=0;
    void add(const nt::graphics::Packet& packet) {
        const auto began=std::chrono::steady_clock::now();
        hash=(hash^nt::fingerprint({reinterpret_cast<const std::uint8_t*>(packet.data()),packet.size()*sizeof(packet[0])}))*0x100000001b3ULL;
        ++packets; quadwords+=packet.size();
        hash_ns+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
    }
    static bool submit(void* raw,std::span<const nt::graphics::Packet> packets) {
        for (const auto& packet:packets) static_cast<PacketReplaySink*>(raw)->add(packet);
        return true;
    }
    static bool take(void* raw,std::vector<nt::graphics::Packet>& packets) {
        for (const auto& packet:packets) static_cast<PacketReplaySink*>(raw)->add(packet);
        packets.clear();
        return true;
    }
    static bool fence(void*) { return true; }
};

// Runs a recorded capture through the real packet path and compiled VU programs, and
// reports what the program's thread pays. Each submission finishes before the next, with
// that wait uncounted, so the costs exclude queueing. Nothing delivers interrupts, so each
// stall is released as a handler would release it.
inline int replay_packets(const std::filesystem::path& path,nt::ee::NativePlatform& platform,nt::ee::Memory& memory,
                          const PacketReplaySink& sink,std::ostream& output) {
    using clock=std::chrono::steady_clock;
    const auto nanoseconds=[](clock::duration elapsed) { return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()); };
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open packet capture");
    std::vector<char> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0); file.read(data.data(),std::streamsize(data.size()));
    auto& setup=platform.packet_setup;
    setup.interrupt_latency={};
    std::uint64_t kicks=0,resumes=0,bytes=0,resume_ns=0,complete_ns=0,longest_ns=0;
    std::array<std::uint64_t,3> consume_ns{},channel_kicks{};
    const auto stopped=[&](nt::ee::Stop stop,const char* where) {
        output<<"packet_replay_stop where="<<where<<" stop="<<unsigned(stop)<<" kick="<<kicks<<'\n';
        return 1;
    };
    // Only the poll that publishes the job's results is counted.
    const auto drain=[&] {
        for (;;) {
            const auto hashed=sink.hash_ns;
            const auto began=clock::now();
            const auto result=platform.poll_submissions();
            const auto elapsed=nanoseconds(clock::now()-began);
            setup.take_interrupts();
            if (result!=nt::ee::Stop::budget) return result;
            if (!setup.working()) { complete_ns+=elapsed-std::min(elapsed,sink.hash_ns-hashed); return result; }
            std::this_thread::yield();
        }
    };
    const auto started=clock::now();
    for (std::size_t offset=0;offset+12<=data.size();) {
        std::uint32_t header[3];
        std::memcpy(header,data.data()+offset,sizeof(header)); offset+=sizeof(header);
        std::vector<nt::ee::NativePacketOrigin> origins(header[2]);
        std::memcpy(origins.data(),data.data()+offset,origins.size()*sizeof(origins[0])); offset+=origins.size()*sizeof(origins[0]);
        const std::span payload(reinterpret_cast<const std::uint8_t*>(data.data()+offset),header[1]); offset+=header[1];
        const auto hashed=sink.hash_ns;
        const auto began=clock::now();
        const auto result=setup.consume(header[0],payload,false,origins);
        const auto taken=nanoseconds(clock::now()-began),elapsed=taken-std::min(taken,sink.hash_ns-hashed);
        if (result!=nt::ee::Stop::budget) return stopped(result,"consume");
        if (header[0]>=3) return stopped(nt::ee::Stop::invalid_state,"channel");
        consume_ns[header[0]]+=elapsed; ++channel_kicks[header[0]]; longest_ns=std::max(longest_ns,elapsed);
        ++kicks; bytes+=payload.size();
        if (const auto drained=drain();drained!=nt::ee::Stop::budget) return stopped(drained,"poll");
        for (unsigned channel=0;channel<2;++channel) while (setup.pending(channel)) {
            const auto resume_hashed=sink.hash_ns;
            const auto resume_began=clock::now();
            const auto released=memory.io.write32(memory.io.context,channel ? 0x10003c10 : 0x10003810,8);
            const auto resume_taken=nanoseconds(clock::now()-resume_began),resume_elapsed=resume_taken-std::min(resume_taken,sink.hash_ns-resume_hashed);
            if (released!=nt::ee::Stop::budget) return stopped(released,"release");
            resume_ns+=resume_elapsed; longest_ns=std::max(longest_ns,resume_elapsed);
            ++resumes;
            if (const auto drained=drain();drained!=nt::ee::Stop::budget) return stopped(drained,"poll");
        }
    }
    const auto wall=nanoseconds(clock::now()-started);
    const auto total_ns=consume_ns[0]+consume_ns[1]+consume_ns[2]+resume_ns+complete_ns;
    output<<"packet_replay kicks="<<kicks<<" resumes="<<resumes<<" bytes="<<bytes
          <<" vu0_ms="<<consume_ns[0]/1e6<<" vu1_ms="<<consume_ns[1]/1e6<<" gif_ms="<<consume_ns[2]/1e6
          <<" vu1_kicks="<<channel_kicks[1]<<" gif_kicks="<<channel_kicks[2]
          <<" resume_ms="<<resume_ns/1e6<<" complete_ms="<<complete_ns/1e6<<" total_ms="<<total_ns/1e6
          <<" longest_ms="<<longest_ns/1e6<<" wall_ms="<<wall/1e6
          <<" packets="<<sink.packets<<" quadwords="<<sink.quadwords<<" hash=0x"<<std::hex<<sink.hash<<std::dec<<'\n';
    return 0;
}
}
