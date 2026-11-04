#include <nt/native_drawing.hpp>
#include <nt/vector_region.hpp>
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Original synthetic vertex regions (emit_vector_region.cpp): AOT boundary code,
// element kernels, range guards and conversion-time PACKED bindings. Each region
// is drawn twice: executed on the CPU and kicked as a packet, and launched with
// GPU-owned steady outputs. Pixels and materialized exit state must match.
extern "C" const nt::render::VectorRegion region_0,region_1,region_2,region_3,region_4;
#define PACKET(n) extern "C" const nt::graphics::VectorRegister region_##n##_packet[]; \
    extern "C" const std::uint32_t region_##n##_packet_count,region_##n##_packet_words;
PACKET(0) PACKET(1) PACKET(2) PACKET(3) PACKET(4)
#undef PACKET
namespace {
using namespace nt::graphics;
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint32_t> shader(const char* path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    const auto size=input.tellg();
    require(size>=20 && size%4==0 && size<16*1024*1024,"Invalid shader fixture");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size)/4);
    input.seekg(0); input.read(reinterpret_cast<char*>(words.data()),size);
    require(bool(input),"Cannot read shader fixture"); return words;
}
struct Binding { const nt::render::VectorRegion* region; const VectorRegister* layout; std::uint32_t count,words; };
Packet registers(std::initializer_list<std::pair<unsigned,std::uint64_t>> values) {
    Packet packet{{0x8000U|static_cast<unsigned>(values.size()),0x10000000,14,0}};
    for (auto [reg,value]:values) packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
    return packet;
}
void setup(NativeDrawing& drawing,VulkanRenderer& renderer,unsigned primitive) {
    const auto packet=registers({{0x1a,1},{0,primitive|8|64},{0x4c,1ULL<<16},{0x18,32ULL|(48ULL<<32)},
        {0x40,0ULL|(63ULL<<16)|(0ULL<<32)|(63ULL<<48)},{0x42,0x44},{0x46,1},{0x4e,8},{0x47,0}});
    drawing.submit(renderer,std::span(&packet,1));
}
// Entry state of the fixture loop: VI3 positions, VI5 colors, VI4 output records,
// VI9 countdown. Unrelated registers and memory are random so that live-out and
// untouched-memory comparisons are meaningful.
nt::vu::State entry(const nt::render::VectorRegion& region,unsigned count,unsigned seed) {
    nt::vu::State s;
    std::mt19937 random(seed);
    for (auto& vector:s.vectors) for (auto& word:vector) word=random();
    for (auto& row:s.data) for (auto& word:row) word=random();
    for (auto& value:s.integers) value=std::uint16_t(random()%20);
    s.integers[2]=8; s.integers[4]=72; s.integers[3]=136; s.integers[8]=std::uint16_t(count-region.trip_offset);
    s.next_pair=region.head;
    // Finite operands for the arithmetic variant: VF7 = VF1*1 + color*2, where
    // small integer color words are zero as floats, so positions are unchanged.
    for (unsigned lane=0;lane<4;++lane) { s.vectors[19][lane]=0x3f800000U; s.vectors[20][lane]=0x40000000U; }
    for (unsigned i=0;i<count;++i) {
        const float position[4]{float(2+random()%59)+0.25f*float(random()%4),float(3+random()%58),float(i+1),0};
        for (unsigned lane=0;lane<4;++lane) {
            s.data[8+i][lane]=std::bit_cast<std::uint32_t>(position[lane]);
            s.data[72+i][lane]=lane==3 ? 64U : random()%256;
        }
    }
    return s;
}
Packet kick(const nt::vu::State& s,unsigned count) {
    Packet packet{{0x8000U|count,0x20000000,0x51,0}};
    for (unsigned i=0;i<2*count;++i) packet.push_back(s.data[136+i]);
    return packet;
}
unsigned compare(const Binding& binding,unsigned kernel,VulkanRenderer& gpu,VulkanRenderer& cpu) {
    const auto& region=*binding.region;
    require(binding.count==2 && binding.words==region.stores+1+(region.flags ? 1U : 0U),"Region packet binding does not match its kernel");
    unsigned cases=0;
    for (unsigned primitive:{3U,4U,5U}) for (unsigned count:{1U,2U,3U,4U,7U,20U,60U}) {
        const auto start=entry(region,count,primitive*100+count);
        auto executed=start;
        nt::render::execute_vector_region(region,executed);
        NativeDrawing generated(true),packets(true);
        setup(generated,gpu,primitive); setup(packets,cpu,primitive);
        const auto packet=kick(executed,count);
        packets.submit(cpu,std::span(&packet,1));
        const auto launch=nt::render::prepare_region_launch(region,start);
        require(launch.elements==count,"Region launch does not cover every element");
        const std::array writes{VectorMemoryWrite{0,launch.memory,true}};
        const VectorLaunch draw{kernel,writes,launch.parameters,launch.elements,binding.words,
            std::span(binding.layout,binding.count),launch.prepared.execution.elements ? region.guard : nullptr,launch.prefix};
        auto state=launch.prepared.state;
        if (count<3) {
            // Fewer vertices than a triangle would leave GPU-owned GS queue state:
            // such a batch is rejected, and the caller materializes and kicks it.
            bool rejected=false;
            try { generated.submit_vectors(gpu,draw); } catch (const std::invalid_argument&) { rejected=true; }
            require(rejected,"Sub-triangle region batch was accepted for GPU drawing");
            nt::render::materialize_region_outputs(region,launch,state);
            require(state==executed,"Materialized short region state differs from CPU execution");
            const auto fallback=kick(state,count);
            generated.submit(gpu,std::span(&fallback,1));
        } else generated.submit_vectors(gpu,draw);
        const auto& actual=generated.pixels(); const auto& expected=packets.pixels();
        require(count<3 || std::any_of(expected.begin(),expected.end(),[](auto x){return x!=0;}),
            "Region comparison produced only an empty image");
        require(actual==expected,"Region pixels differ: unit "+std::to_string(region.unit)+" startup "+
            std::to_string(region.startup)+" primitive "+std::to_string(primitive)+" count "+std::to_string(count));
        // Steady rows stay GPU-owned until a non-graphics consumer materializes them.
        state=launch.prepared.state;
        nt::render::materialize_region_outputs(region,launch,state);
        require(state==executed,"Materialized region state differs from CPU execution");
        ++cases;
    }
    return cases;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==6,"Usage: render-region-draw-tests <region-0.spv> ... <region-4.spv>");
        const Binding bindings[]{{&region_0,region_0_packet,region_0_packet_count,region_0_packet_words},
            {&region_1,region_1_packet,region_1_packet_count,region_1_packet_words},
            {&region_2,region_2_packet,region_2_packet_count,region_2_packet_words},
            {&region_3,region_3_packet,region_3_packet_count,region_3_packet_words},
            {&region_4,region_4_packet,region_4_packet_count,region_4_packet_words}};
        VulkanRenderer gpu(nullptr,true),cpu(nullptr,true);
        unsigned cases=0;
        for (unsigned variant=0;variant<5;++variant) {
            const auto kernel=gpu.load_vector_kernel(shader(argv[variant+1]),bindings[variant].region->unit);
            cases+=compare(bindings[variant],kernel,gpu,cpu);
        }
        gpu.synchronize(); cpu.synchronize();
        require(gpu.statistics().vector_failures==0,"Valid regions reported failed elements");
        require(!gpu.validation_errors(),gpu.validation_message());
        require(!cpu.validation_errors(),cpu.validation_message());
        std::cout<<cases<<" GPU-owned region draws match CPU execution and packet pixels on "<<gpu.device_name()<<'\n';
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
