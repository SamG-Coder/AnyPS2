#include <nt/vector_region.hpp>
#if defined(ANYPS2_REGION_VULKAN)
#include "vulkan_compute.hpp"
#endif
#include <algorithm>
#include <bit>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" const nt::render::VectorRegion region_0,region_1,region_2,region_3,region_4,region_5;
namespace {
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
nt::vu::State initial(const nt::render::VectorRegion& region,unsigned count,unsigned seed) {
    nt::vu::State s;
    std::mt19937 random(seed);
    for (auto& vector:s.vectors) for (auto& word:vector) word=random();
    for (auto& row:s.data) for (auto& word:row) word=random();
    for (auto& word:s.accumulator) word=random();
    for (auto& value:s.integers) value=std::uint16_t(random()%20);
    // The counter value that makes the original loop run `count` iterations.
    s.integers[2]=8; s.integers[3]=136; s.integers[4]=64; s.integers[8]=std::uint16_t(count-region.trip_offset);
    s.next_pair=region.head; s.immediate=random(); s.quotient=random(); s.scalar_p=random();
    s.mac=random()&65535; s.status=random()&4095; s.clip=random()&0xffffff;
    s.visible_mac=s.mac; s.visible_status=s.status; s.visible_clip=s.clip;
    s.pending_quotient=random(); s.quotient_flags=random()&0x30; s.pending_p=random();
    s.delayed_value=std::uint16_t(random()); s.graphics_address=random(); s.graphics_completes=random()&1;
    for (unsigned i=0;i<4;++i) { s.pending_mac[i]=random(); s.pending_clip[i]=random(); s.pending_product_sticky[i]=random(); }
    // Finite positions, distinct colors; all unrelated memory remains a canary.
    for (unsigned i=0;i<count;++i) for (unsigned lane=0;lane<4;++lane)
        s.data[8+i][lane]=std::bit_cast<std::uint32_t>(float(int(random()%100)-50)/4.0f);
    if (region.flags) {
        // Finite arithmetic operands. One steady element overflows its product,
        // so the overflow sticky bit can only come from the element kernel.
        for (unsigned lane=0;lane<4;++lane) { s.vectors[19][lane]=0x3f800000U; s.vectors[20][lane]=0x40000000U; }
        for (unsigned i=0;i<count;++i) for (unsigned lane=0;lane<4;++lane)
            s.data[64+i][lane]=std::bit_cast<std::uint32_t>(float(int(random()%64)-32));
        if (count>=7) s.data[64+count/2][1]=0x7f000000U;
        s.status&=~0xfc0U; s.visible_status=s.status;
    }
    return s;
}
nt::vu::State reference(const nt::render::VectorRegion& region,nt::vu::State s,unsigned count) {
    for (unsigned i=0;i<count;++i) {
        require(region.iteration(&s)==nt::vu::Result::transfer,"Original compiled iteration failed");
        require(s.next_pair==(i+1==count ? region.exit:region.head),"Original loop count differs");
    }
    return s;
}
void compare(const nt::vu::State& actual,const nt::vu::State& expected) {
    require(actual.vectors==expected.vectors,"Region live-out VF lanes differ");
    require(actual.integers==expected.integers,"Region live-out VI values differ");
    require(actual.data==expected.data,"Region startup/steady/drain output or untouched memory differs");
    require(actual==expected,"Region flags, pipeline or continuation state differs");
}
template<class F> void rejects_unchanged(nt::vu::State& s,F&& operation) {
    const auto before=s; bool rejected=false;
    try { operation(); } catch (const std::exception&) { rejected=true; }
    require(rejected,"Unsafe region entry accepted"); require(s==before,"Rejected region changed entry state");
}
nt::vu::Result failed_boundary(nt::vu::State* s) {
    s->vectors[0][0]^=0x1234;
    return nt::vu::Result::unsupported_numeric;
}
void failed_kernel(void*,std::span<const std::uint32_t>,std::span<const std::uint32_t>,std::span<std::uint32_t> output) {
    std::fill(output.begin(),output.end(),0); output.back()=0; output[0]=1;
}
void gpu_kernel(void* context,std::span<const std::uint32_t> data,std::span<const std::uint32_t> parameters,std::span<std::uint32_t> output) {
#if defined(ANYPS2_REGION_VULKAN)
    const auto& shader=*static_cast<const std::vector<std::uint32_t>*>(context);
    std::vector<std::uint32_t> memory(data.begin(),data.end()),params(parameters.begin(),parameters.end());
    const std::span<std::uint32_t> buffers[]{memory,output,params};
    vulkan_compute::Device device; device.initialize();
    device.run(shader,buffers,(parameters[0]+63)/64);
    require(std::equal(memory.begin(),memory.end(),data.begin()),"GPU region changed inputs");
    require(std::equal(params.begin(),params.end(),parameters.begin()),"GPU region changed parameters");
#else
    throw std::runtime_error("Vulkan region test not configured");
#endif
}
}
int main(int argc,char** argv) {
    try {
        require(argc==1 || argc==7,"Expected zero or six AOT shader paths");
        const nt::render::VectorRegion* regions[]{&region_0,&region_1,&region_2,&region_3,&region_4,&region_5};
        unsigned checks=0;
        for (unsigned variant=0;variant<6;++variant) {
            const auto& region=*regions[variant];
            for (unsigned count:{1U,2U,3U,4U,7U,20U,60U}) for (unsigned seed=0;seed<8;++seed) {
                auto state=initial(region,count,seed);
                const auto expected=reference(region,state,count);
                const auto stats=nt::render::execute_vector_region(region,state);
                compare(state,expected); ++checks;
                if (count>region.startup+region.drain)
                    require(stats.startup==region.startup && stats.drain==region.drain && stats.elements==count-region.startup,
                        "Region replayed the entire loop instead of the proved boundaries");
                else require(stats.startup==count && !stats.elements && !stats.drain,"Short loop was incorrectly partitioned");
            }
            for (unsigned fault=0;fault<6;++fault) {
                auto state=initial(region,20,7);
                if (fault==0) state.next_pair++;
                if (fault==1) state.quotient_pending=1;
                if (fault==2) state.integers[2]=state.integers[3];
                // Zero is a valid one-iteration count when the backed-up counter is compared.
                if (fault==3) state.integers[8]=region.trip_offset ? 1000 : 0;
                if (fault==4) state.p_pending=1; // Element kernels read P from the entry.
                if (fault==5) state.integers[3]=std::uint16_t(region.unit ? 1020:250);
                rejects_unchanged(state,[&]{nt::render::execute_vector_region(region,state);});
            }
            // Heads reached during execution carry the loop's own in-flight FMAC
            // results and integer branch backup; regions absorb them exactly.
            for (unsigned warm:{1U,2U}) for (unsigned count:{3U,7U,20U}) {
                auto entered=initial(region,count+warm,warm+90);
                for (unsigned i=0;i<warm;++i) require(region.iteration(&entered)==nt::vu::Result::transfer,"Warm-up iteration failed");
                const auto expected=reference(region,entered,count);
                nt::render::execute_vector_region(region,entered);
                compare(entered,expected); ++checks;
            }
            auto state=initial(region,20,8);
            rejects_unchanged(state,[&]{nt::render::execute_vector_region(region,state,failed_kernel);});
            auto broken=region; broken.iteration=failed_boundary;
            rejects_unchanged(state,[&]{nt::render::execute_vector_region(broken,state);});
#if defined(ANYPS2_REGION_VULKAN)
            if (argc==7) {
                auto shader=vulkan_compute::read_shader(argv[variant+1]);
                for (unsigned count:{7U,20U,60U}) {
                    state=initial(region,count,variant+42);
                    const auto expected=reference(region,state,count);
                    nt::render::execute_vector_region(region,state,gpu_kernel,&shader);
                    compare(state,expected); ++checks;
                }
            }
#endif
        }
        std::cout<<checks<<" complete region states match original AOT VU loops; entry and rollback checks passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
