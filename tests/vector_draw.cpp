#include <nt/native_drawing.hpp>
#include <nt/loop_pipeline.hpp>
#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

// Original synthetic VU vertex loop, AOT-compiled for the host and Vulkan.
extern "C" const nt::graphics::VectorRegister nt_render_vertex_layout[];
extern "C" const std::uint32_t nt_render_vertex_layout_count,nt_render_vertex_layout_words,nt_render_vertex_layout_minimum_element;
static_assert(sizeof(nt::graphics::VectorRegister)==9*sizeof(std::uint32_t));
extern "C" std::uint32_t nt_render_vertex_guard(const std::uint32_t*);
extern "C" std::uint32_t nt_render_pipelined_guard(const std::uint32_t*);
extern "C" std::uint32_t nt_render_step2_guard(const std::uint32_t*);
extern "C" void nt_render_elements(const std::uint32_t*,const std::uint32_t*,std::uint32_t*);
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
Packet registers(std::initializer_list<std::pair<unsigned,std::uint64_t>> values) {
    Packet packet{{0x8000U|static_cast<unsigned>(values.size()),0x10000000,14,0}};
    for (auto [reg,value]:values) packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
    return packet;
}
void submit(NativeDrawing& drawing,VulkanRenderer& renderer,const Packet& packet) { drawing.submit(renderer,std::span(&packet,1)); }
void setup(NativeDrawing& drawing,VulkanRenderer& renderer,unsigned primitive,bool shaded=true,bool depth=false,bool coverage=false) {
    submit(drawing,renderer,registers({{0x1a,1},{0,primitive|(shaded ? 8:0)|(coverage ? 128:0)|64},
        {0x4c,1ULL<<16},{0x18,32ULL|(48ULL<<32)},
        {0x40,2ULL|(61ULL<<16)|(3ULL<<32)|(60ULL<<48)},
        {0x42,0x44},{0x46,1},{0x4e,8},{0x47,depth ? 0x70000U:0U}}));
}
struct Fixture {
    static constexpr unsigned count=12;
    std::vector<std::uint32_t> memory=std::vector<std::uint32_t>(4096);
    std::vector<std::uint32_t> parameters,output=std::vector<std::uint32_t>(count*9);
    std::vector<VectorRegister> layout;
    std::array<VectorMemoryWrite,2> writes;
    Fixture():layout(nt_render_vertex_layout,nt_render_vertex_layout+nt_render_vertex_layout_count) {
        require(layout.size()==2 && nt_render_vertex_layout_words==9 && nt_render_vertex_layout_minimum_element==0,
            "Compiled element binding does not match the fixture ABI");
        const float xy[count][2]{{5.25f,5},{28,6.5f},{6,29},{36,8},{58,27},{33,30},
            {5,40},{18,58},{30,39},{45,42},{55,57},{62,38}};
        for (unsigned i=0;i<count;++i) {
            const std::array<float,4> position{xy[i][0]+2,xy[i][1]+3,float(i+1),0};
            for (unsigned lane=0;lane<4;++lane) memory[(100+i)*4+lane]=std::bit_cast<std::uint32_t>(position[lane]);
            memory[(200+i)*4]=(i*53+40)%256;
            memory[(200+i)*4+1]=(i*39+70)%256;
            memory[(200+i)*4+2]=(i*21+100)%256;
            memory[(200+i)*4+3]=64;
        }
        nt::render::ElementEntry entry;
        entry.integers[3]=100; entry.integers[5]=200; entry.integers[4]=300; entry.integers[9]=count;
        parameters=nt::render::element_parameters(entry,0,count);
        writes={VectorMemoryWrite{100,std::span(memory).subspan(400,count*4),true},
            VectorMemoryWrite{200,std::span(memory).subspan(800,count*4),false}};
        compile_cpu();
    }
    void compile_cpu() { nt_render_elements(memory.data(),parameters.data(),output.data()); }
    VectorLaunch launch(unsigned kernel) const { return {kernel,writes,parameters,count,9,layout,nt_render_vertex_guard}; }
    Packet packet(unsigned xyz=5) const {
        Packet packet{{0x8000U|count,0x20000000,1U|(xyz<<4),0}};
        for (unsigned i=0;i<count;++i) {
            require(output[i*9]==0,"Vertex fixture reported unsupported arithmetic");
            for (unsigned reg=0;reg<2;++reg) {
                Quadword packed{};
                for (unsigned lane=0;lane<4;++lane) {
                    const auto index=layout[reg].store[lane];
                    packed[lane]=index==vector_constant_word ? layout[reg].constant[lane] : output[i*9+1+index];
                    const auto expected=reg==0 ? memory[(200+i)*4+lane] :
                        static_cast<std::uint32_t>(std::bit_cast<float>(memory[(100+i)*4+lane])*16.0f);
                    require(packed[lane]==expected,"Compiled mapping does not reconstruct the original PACKED word");
                }
                packet.push_back(packed);
            }
        }
        return packet;
    }
};
template<class F> void rejects(F&& operation,const char* diagnostic) {
    try { operation(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(diagnostic)!=std::string::npos,
            std::string("Wrong diagnostic: ")+error.what()); return;
    }
    throw std::runtime_error(std::string("Accepted unsupported batch: ")+diagnostic);
}
void compare(VulkanRenderer& gpu,VulkanRenderer& cpu,unsigned kernel) {
    unsigned cases=0;
    for (unsigned primitive:{3U,4U,5U}) for (unsigned variant=0;variant<6;++variant) {
        Fixture fixture;
        if (variant==1) { // ADC updates the queue but suppresses only the kicking vertex.
            fixture.memory[(100+2)*4+3]=std::bit_cast<std::uint32_t>(2048.0f);
            fixture.memory[(100+7)*4+3]=std::bit_cast<std::uint32_t>(2048.0f);
        }
        if (variant==2) { // Degenerate, reversed and fully off-scissor triangles.
            for (unsigned lane=0;lane<2;++lane) fixture.memory[(100+1)*4+lane]=fixture.memory[100*4+lane];
            for (unsigned i=3;i<6;++i) fixture.memory[(100+i)*4]=std::bit_cast<std::uint32_t>(100.0f+float(i));
        }
        if (variant==3) fixture.layout[1].reg=13; // XYZ3 never kicks.
        fixture.compile_cpu();
        NativeDrawing generated(true),packets(true);
        setup(generated,gpu,primitive,variant!=4,variant==5,variant==4);
        setup(packets,cpu,primitive,variant!=4,variant==5,variant==4);
        const auto draws=gpu.statistics().draw_calls;
        generated.submit_vectors(gpu,fixture.launch(kernel));
        submit(packets,cpu,fixture.packet(fixture.layout[1].reg));
        const auto& actual=generated.pixels(); const auto& expected=packets.pixels();
        const auto mismatch=std::mismatch(actual.begin(),actual.end(),expected.begin());
        require(mismatch.first==actual.end(),"Pixels differ for primitive "+std::to_string(primitive)+
            " variant "+std::to_string(variant)+" word "+std::to_string(mismatch.first-actual.begin()));
        require(gpu.statistics().draw_calls==draws+1,"Vector launch was not one instanced draw");
        if (variant==3) require(std::all_of(actual.begin(),actual.end(),[](auto x){return x==0;}),"XYZ3 drew fragments");
        else require(std::any_of(actual.begin(),actual.end(),[](auto x){return x!=0;}),"Comparison produced only an empty image");
        const auto hits=gpu.statistics().vector_resident_hits,uploads=gpu.statistics().vector_resident_uploads;
        // Restart the primitive, retain resident geometry, update only colors.
        fixture.memory[800]^=0x7f; fixture.compile_cpu();
        setup(generated,gpu,primitive,variant!=4,variant==5,variant==4);
        setup(packets,cpu,primitive,variant!=4,variant==5,variant==4);
        generated.submit_vectors(gpu,fixture.launch(kernel));
        submit(packets,cpu,fixture.packet(fixture.layout[1].reg));
        require(generated.pixels()==packets.pixels(),"Second-frame resident pixels differ");
        require(gpu.statistics().vector_resident_hits==hits+1 && gpu.statistics().vector_resident_uploads==uploads,
            "Second frame reuploaded resident geometry");
        // CPU -> GPU -> CPU on the same target without a readback boundary.
        setup(generated,gpu,primitive,variant!=4,variant==5,variant==4);
        setup(packets,cpu,primitive,variant!=4,variant==5,variant==4);
        submit(generated,gpu,fixture.packet(fixture.layout[1].reg));
        submit(packets,cpu,fixture.packet(fixture.layout[1].reg));
        setup(generated,gpu,primitive,variant!=4,variant==5,variant==4);
        setup(packets,cpu,primitive,variant!=4,variant==5,variant==4);
        generated.submit_vectors(gpu,fixture.launch(kernel));
        submit(packets,cpu,fixture.packet(fixture.layout[1].reg));
        setup(generated,gpu,primitive,variant!=4,variant==5,variant==4);
        setup(packets,cpu,primitive,variant!=4,variant==5,variant==4);
        submit(generated,gpu,fixture.packet(fixture.layout[1].reg));
        submit(packets,cpu,fixture.packet(fixture.layout[1].reg));
        require(generated.pixels()==packets.pixels(),"Mixed CPU/GPU ordering differs");
        ++cases;
    }
    std::cout<<cases<<" list/strip/fan cases and second frames match CPU packet pixels\n";
}
void ranges(VulkanRenderer& gpu,unsigned kernel) {
    Fixture fixture;
    const auto original=fixture.parameters;
    NativeDrawing drawing(true);
    setup(drawing,gpu,3);
    const auto launches=gpu.statistics().vector_launches;
    for (auto [index,value,status,diagnostic]:{
            std::tuple{5U,300U,VectorRangeStatus::alias,"ranges alias"},
            {7U,299U,VectorRangeStatus::alias,"ranges alias"},
            {5U,1020U,VectorRangeStatus::wrap,"range wraps"},
            {6U,1010U,VectorRangeStatus::wrap,"range wraps"},
            {10U,65536U,VectorRangeStatus::wrap,"range wraps"},
            {11U,0U,VectorRangeStatus::count,"loop count"},
            {11U,11U,VectorRangeStatus::count,"loop count"},
            {1U,1U,VectorRangeStatus::count,"loop count"}}) {
        fixture.parameters=original; fixture.parameters[index]=value;
        require(nt_render_vertex_guard(fixture.parameters.data())==static_cast<unsigned>(status),"Compiled range guard returned wrong status");
        rejects([&]{drawing.submit_vectors(gpu,fixture.launch(kernel));},diagnostic);
    }
    require(gpu.statistics().vector_launches==launches,"Invalid range recorded GPU work");
    auto parameters=original;
    parameters[11]=24;
    require(nt_render_step2_guard(parameters.data())==0,"Divisible countdown rejected");
    parameters[11]=25;
    require(nt_render_step2_guard(parameters.data())==unsigned(VectorRangeStatus::count),"Nonterminating countdown accepted");
    // Exercise inclusive memory boundaries and subranges through the compiled guard.
    for (unsigned count:{1U,3U,12U,20U}) {
        parameters=original; parameters[0]=count; parameters[11]=count;
        parameters[5]=1024-count; parameters[6]=400; parameters[7]=100;
        require(nt_render_vertex_guard(parameters.data())==0,"Disjoint range ending at last row rejected");
        parameters[5]++;
        require(nt_render_vertex_guard(parameters.data())==unsigned(VectorRangeStatus::wrap),"Wrapped final load accepted");
    }
    parameters=original; parameters[0]=6; parameters[1]=6;
    require(nt_render_vertex_guard(parameters.data())==0,"Valid steady subrange rejected");
    parameters=original;
    require(nt_render_pipelined_guard(parameters.data())==unsigned(VectorRangeStatus::startup),"Unrecovered startup accepted");
    parameters[0]=11; parameters[1]=1;
    require(nt_render_pipelined_guard(parameters.data())==0,"Pipelined steady subrange rejected");
    parameters[5]=0;
    require(nt_render_pipelined_guard(parameters.data())==0,"Legal previous-iteration load incorrectly treated as wrapped");
    parameters[0]=0;
    require(nt_render_vertex_guard(parameters.data())==unsigned(VectorRangeStatus::count),"Empty range accepted");
}
void contracts(VulkanRenderer& gpu,unsigned kernel) {
    NativeDrawing drawing(true);
    Fixture fixture;
    setup(drawing,gpu,4);
    drawing.submit_vectors(gpu,fixture.launch(kernel));
    rejects([&]{drawing.submit_vectors(gpu,fixture.launch(kernel));},"continue an existing primitive");
    // Merely resetting PRIM does not restore the GPU-written color latch.
    submit(drawing,gpu,registers({{0,3}}));
    rejects([&]{submit(drawing,gpu,registers({{5,16ULL|(16ULL<<16)}}));},"GPU-owned GS state");
    submit(drawing,gpu,registers({{1,0x3f800000aabbccddULL}}));
    submit(drawing,gpu,registers({{5,16ULL|(16ULL<<16)}}));
    rejects([&]{drawing.submit_vectors(gpu,fixture.launch(kernel));},"continue an existing primitive");
    setup(drawing,gpu,3);
    const auto launches=gpu.statistics().vector_launches;
    for (auto [bits,reason]:{std::pair{16U,"Textured"},std::pair{32U,"Fogged"}}) {
        submit(drawing,gpu,registers({{0,3U|bits}}));
        rejects([&]{drawing.submit_vectors(gpu,fixture.launch(kernel));},reason);
    }
    setup(drawing,gpu,3);
    auto bad=fixture.launch(kernel); bad.kernel=~0U;
    rejects([&]{drawing.submit_vectors(gpu,bad);},"Unknown vector element kernel");
    fixture.layout[0].store[0]=0xfffffffeU;
    rejects([&]{drawing.submit_vectors(gpu,fixture.launch(kernel));},"missing store word");
    fixture.layout[0].store[0]=nt_render_vertex_layout[0].store[0];
    require(gpu.statistics().vector_launches==launches,"Rejected batch recorded GPU work");
    // All vertex latches written by a GPU record become unknown, even when
    // this untextured draw does not use ST/UV/FOG. Restore them explicitly.
    VectorRegister st; st.reg=2; st.constant={0,0,0x3f800000,0};
    VectorRegister uv; uv.reg=3; uv.constant={16,32,0,0};
    VectorRegister fog; fog.reg=10; fog.constant={0,0,0,128U<<4};
    std::array extended{st,fixture.layout[0],uv,fog,fixture.layout[1]};
    auto full=fixture.launch(kernel); full.vertex=extended;
    drawing.submit_vectors(gpu,full);
    for (auto [reg,value]:{std::pair<unsigned,std::uint64_t>{1,0x3f800000aabbccddULL},
            {2,0}, {3,0}}) {
        submit(drawing,gpu,registers({{reg,value}}));
        rejects([&]{submit(drawing,gpu,registers({{5,16ULL|(16ULL<<16)}}));},"GPU-owned GS state");
    }
    submit(drawing,gpu,registers({{10,0}}));
    submit(drawing,gpu,registers({{5,16ULL|(16ULL<<16)}}));
    setup(drawing,gpu,3);
    std::swap(extended[0],extended[1]);
    rejects([&]{drawing.submit_vectors(gpu,full);},"ST must precede RGBAQ");
    // More than one slot's record capacity, without intervening readback. This
    // exercises fence collection, slot wrap and compute after CPU draws.
    for (unsigned n=0;n<1030;++n) {
        setup(drawing,gpu,3);
        drawing.submit_vectors(gpu,fixture.launch(kernel));
    }
    gpu.synchronize();
    require(gpu.statistics().vector_failures==0,"Valid slot reuse reported failures");
}
void failures(VulkanRenderer& gpu,unsigned quotient) {
    NativeDrawing drawing(true);
    setup(drawing,gpu,3);
    nt::render::ElementEntry entry; entry.integers[3]=100;
    auto parameters=nt::render::element_parameters(entry,0,3);
    std::array<std::uint32_t,12> input{0x3f800000,0x3f800000,0x3f800080,0x3f800080,
        0x3f800000,0x3f800000,0x3f800300,0x3f800080,0x3f800000,0x3f800000,0x3f800080,0x3f800300};
    const std::array<VectorMemoryWrite,1> writes{{{100,input,false}}};
    VectorRegister color; color.reg=1; color.constant={200,80,40,128};
    VectorRegister xyz; xyz.reg=5; xyz.store={2,3,vector_constant_word,vector_constant_word};
    const std::array layout{color,xyz};
    const VectorLaunch launch{quotient,writes,parameters,3,6,layout};
    drawing.submit_vectors(gpu,launch);
    const auto good=drawing.pixels();
    require(std::any_of(good.begin(),good.end(),[](auto x){return x!=0;}),"Failure fixture cannot draw a valid triangle");
    const auto failures=gpu.statistics().vector_failures;
    input[0]=input[4]=input[8]=0x7f800000;
    // Different color would change the image if failed elements were consumed.
    auto invalid_layout=layout; invalid_layout[0].constant={10,250,10,128};
    auto invalid=launch; invalid.vertex=invalid_layout;
    for (unsigned frame=0;frame<5;++frame) {
        setup(drawing,gpu,3); drawing.submit_vectors(gpu,invalid); gpu.enqueue_finish();
    }
    gpu.synchronize();
    require(gpu.statistics().vector_failures==failures+15,"Per-slot failed element counts were lost or double counted");
    require(drawing.pixels()==good,"Failed elements rendered geometry");
    gpu.synchronize();
    require(gpu.statistics().vector_failures==failures+15,"Repeated fence collection double counted failures");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"Usage: render-vector-draw-tests <vertex.spv> <quotient.spv>");
        VulkanRenderer gpu(nullptr,true),cpu(nullptr,true);
        const auto kernel=gpu.load_vector_kernel(shader(argv[1]),1);
        const auto quotient=gpu.load_vector_kernel(shader(argv[2]),1);
        compare(gpu,cpu,kernel); ranges(gpu,kernel); contracts(gpu,kernel); failures(gpu,quotient);
        gpu.synchronize(); cpu.synchronize();
        require(!gpu.validation_errors(),gpu.validation_message());
        require(!cpu.validation_errors(),cpu.validation_message());
        std::cout<<"GPU batches, residency, state invalidation and failures verified on "<<gpu.device_name()<<'\n';
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
