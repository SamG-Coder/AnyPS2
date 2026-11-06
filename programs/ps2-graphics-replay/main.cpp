#include <nt/native_drawing.hpp>
#include <nt/pixel_address.hpp>
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
namespace {
// Writes one memory surface as a binary PPM. PSMCT32 and PSMCT24 ignore alpha;
// PSMCT16 (format 2) and PSMCT16S (format 10) expand to eight bits per channel.
void write_surface(const std::string& request,const std::vector<std::uint32_t>& memory,nt::graphics::VulkanRenderer& renderer) {
    std::vector<std::string> parts;
    std::size_t cursor=request.size();
    for (unsigned field=0;field<5;++field) {
        const auto colon=request.rfind(':',cursor-1);
        if (colon==std::string::npos) throw std::runtime_error("Invalid surface request");
        parts.insert(parts.begin(),request.substr(colon+1,cursor-colon-1));
        cursor=colon;
    }
    const unsigned frame=static_cast<unsigned>(std::stoul(parts[0])),pitch=static_cast<unsigned>(std::stoul(parts[1]));
    const unsigned format=static_cast<unsigned>(std::stoul(parts[2])),width=static_cast<unsigned>(std::stoul(parts[3])),height=static_cast<unsigned>(std::stoul(parts[4]));
    std::ofstream file(request.substr(0,cursor),std::ios::binary);
    if (!file) throw std::runtime_error("Cannot write surface");
    // A scaled surface writes every sample; its pixels are already expanded.
    unsigned sample_width=0,sample_height=0;
    const auto samples=renderer.read_surface_samples(frame*32,pitch,format,sample_width,sample_height);
    if (!samples.empty() && sample_width>=width*3) {
        const unsigned scale_x=sample_width/(pitch*64),out_width=width*scale_x,out_height=std::min(height*scale_x,sample_height);
        file<<"P6\n"<<out_width<<' '<<out_height<<"\n255\n";
        for (unsigned y=0;y<out_height;++y) for (unsigned x=0;x<out_width;++x) {
            const auto pixel=samples[std::size_t(y)*sample_width+x];
            const char rgb[]{char(pixel),char(pixel>>8),char(pixel>>16)};
            file.write(rgb,3);
        }
        return;
    }
    file<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        std::uint32_t pixel;
        if (format==2 || format==10) {
            const auto half=nt::graphics::rgba_halfword(frame*32,pitch,x,y,format==10);
            const auto value=(memory[half/2]>>((half%2)*16))&65535U;
            pixel=((value&31U)<<3)|(((value>>5)&31U)<<11)|(((value>>10)&31U)<<19);
        } else pixel=memory[nt::graphics::rgba_word(frame*32,pitch,x,y)];
        const char rgb[]{char(pixel),char(pixel>>8),char(pixel>>16)};
        file.write(rgb,3);
    }
}
}
int main(int argc,char** argv) {
    try {
        unsigned scale=1;
        if (argc>2 && std::string(argv[1])=="--scale") { scale=static_cast<unsigned>(std::stoul(argv[2])); argc-=2; argv+=2; }
        if (argc<2 || argc>4) { std::cerr<<"usage: ps2-graphics-replay [--scale n] <packets.bin> [repeat] [surface.ppm:fbp:fbw:psm:width:height]\n"; return 2; }
        const auto data=read_all(argv[1]);
        const unsigned repeat=argc>=3 ? static_cast<unsigned>(std::stoul(argv[2])) : 1;
        nt::graphics::VulkanRenderer renderer(nullptr,false,nt::graphics::RenderPath::native_images,scale);
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
        if (argc==4) write_surface(argv[3],memory,renderer);
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
