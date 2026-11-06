#include <nt/native_drawing.hpp>
#include <nt/vulkan_renderer.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Replays a private graphics capture through the native renderer without a game,
// reporting its cost and a hash of the resulting memory. Two builds that hash the
// same on one capture rendered it identically.
namespace {
std::vector<std::uint8_t> read_all(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open capture");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0); file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    return data;
}
struct Reader {
    const std::vector<std::uint8_t>& data; std::size_t position=4;
    bool done() const { return position>=data.size(); }
    std::uint32_t word() {
        if (data.size()-position<4) throw std::runtime_error("Truncated capture");
        std::uint32_t value; std::memcpy(&value,data.data()+position,4); position+=4;
        return value;
    }
};
}
int main(int argc,char** argv) {
    try {
        if (argc<2 || argc>3) { std::cerr<<"usage: ps2-graphics-replay <packets.bin> [repeat]\n"; return 2; }
        const auto data=read_all(argv[1]);
        const unsigned repeat=argc==3 ? static_cast<unsigned>(std::stoul(argv[2])) : 1;
        nt::graphics::VulkanRenderer renderer;
        renderer.profile_commands(true);
        nt::graphics::NativeDrawing drawing(true);
        drawing.assemble_triangles_on_gpu(renderer);
        const auto began=std::chrono::steady_clock::now();
        std::uint64_t updates=0,batches=0,rejected=0; std::string first_rejection;
        for (unsigned pass=0;pass<repeat;++pass) {
            Reader reader{data};
            while (!reader.done()) {
                const auto kind=reader.word();
                if (kind==2) continue;
                if (kind==3) { ++updates; continue; }
                if (kind!=1) throw std::runtime_error("Unknown capture record");
                std::vector<nt::graphics::Packet> packets(reader.word());
                for (auto& packet:packets) {
                    packet.resize(reader.word());
                    const auto bytes=packet.size()*sizeof(nt::graphics::Quadword);
                    if (data.size()-reader.position<bytes) throw std::runtime_error("Truncated capture");
                    std::memcpy(packet.data(),data.data()+reader.position,bytes); reader.position+=bytes;
                }
                // A capture can start mid-scene: batches relying on earlier register state fail
                // until the game restates it, which costs nothing here and changes no hash.
                try { drawing.submit(renderer,packets); ++batches; }
                catch (const std::invalid_argument& error) { if (!rejected++) first_rejection=error.what(); }
            }
        }
        renderer.synchronize();
        const auto recorded=std::chrono::steady_clock::now();
        std::vector<std::uint32_t> memory(1024*1024);
        renderer.read(memory);
        std::uint64_t hash=0xcbf29ce484222325ULL;
        for (const auto word:memory) hash=(hash^word)*0x100000001b3ULL;
        const auto& s=renderer.statistics();
        std::cout<<"updates="<<updates<<" batches="<<batches
                 <<" wall_ms_per_update="<<std::chrono::duration<double,std::milli>(recorded-began).count()/double(updates ? updates : 1)
                 <<" gpu_ms_per_update="<<double(s.gpu_ns)/1e6/double(updates ? updates : 1)
                 <<" wait_ms="<<double(s.wait_ns)/1e6<<" draw_ms="<<double(s.draw_ns)/1e6
                 <<" render_passes="<<s.render_passes<<" ordering_barriers="<<s.ordering_barriers<<" draw_calls="<<s.draw_calls
                 <<" submissions="<<s.submissions<<" gpu_palette_loads="<<s.gpu_palette_loads<<" surface_palette_loads="<<s.surface_palette_loads
                 <<" triangles="<<s.triangles<<" sprites="<<s.sprites<<" uploads="<<s.uploads<<" readback_bytes="<<s.readback_bytes
                 <<" rejected="<<rejected<<" first_rejection=["<<first_rejection<<"]"
                 <<" memory_hash=0x"<<std::hex<<hash<<std::dec<<'\n';
        return 0;
    } catch (const std::exception& error) { std::cerr<<"replay: "<<error.what()<<'\n'; return 1; }
}
