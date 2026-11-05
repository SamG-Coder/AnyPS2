#include "vulkan_compute.hpp"
#include <nt/vu_dispatch.hpp>
#include <nt/vu_gpu.hpp>
#include <nt/graphics_packet.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// The CPU and Vulkan targets of one generated VU1 program must agree on every
// job: status, block count, final state and the GIF packets kicked.
extern "C" const nt::vu::Block* nt_vu_gpu_fixture_blocks();
extern "C" std::uint32_t nt_vu_gpu_fixture_count();
using vulkan_compute::check;
namespace gpu=nt::vu::gpu;

namespace {
constexpr unsigned transform_entry=0,efu_entry=15,scheduled_entry=30;
struct Job {
    std::unique_ptr<nt::vu::State> state=std::make_unique<nt::vu::State>();
    std::uint32_t budget=4096,capacity=64;
    std::string name;
};
struct Outcome {
    gpu::JobStatus status=gpu::JobStatus::not_run;
    std::unique_ptr<nt::vu::State> state;
    std::uint32_t blocks=0;
    std::vector<std::uint32_t> output;
};
std::uint32_t random_word(std::uint32_t& seed) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
// Normal values between 1/16 and 16 keep the programs in the supported range.
std::uint32_t random_float(std::uint32_t& seed) {
    const auto word=random_word(seed);
    return (word&0x80000000U)|((123U+word%9U)<<23)|(random_word(seed)&0x7fffffU);
}
std::uint32_t positive_float(std::uint32_t& seed) { return random_float(seed)&0x7fffffffU; }
void tag(nt::vu::State& state,unsigned row,unsigned loops,bool end) {
    state.data[row]={loops|(end ? 0x8000U : 0U),1U<<28,5,0};
}
Job job(unsigned entry,std::uint32_t seed,unsigned variant) {
    Job result;
    auto& s=*result.state;
    for (auto& vector:s.vectors) for (auto& lane:vector) lane=random_float(seed);
    for (auto& lane:s.accumulator) lane=random_float(seed);
    for (auto& row:s.data) for (auto& word:row) word=random_word(seed);
    s.immediate=random_float(seed); s.quotient=positive_float(seed); s.scalar_p=positive_float(seed);
    s.next_pair=entry;
    for (unsigned i=0;i<15;++i) s.integers[i]=static_cast<std::uint16_t>(random_word(seed)&0x3ff);
    if (entry==transform_entry) {
        const unsigned count=1+variant%8;
        s.integers[0]=100; s.integers[1]=static_cast<std::uint16_t>(count); s.integers[2]=300;
        s.integers[4]=299; s.integers[5]=600; s.integers[8]=500;
        for (unsigned row=100;row<108;++row) {
            for (unsigned lane=0;lane<3;++lane) s.data[row][lane]=random_float(seed);
            s.data[row][3]=0x3f800000U; // w = 1
        }
        // Positive w rows keep the perspective division finite and nonzero.
        for (unsigned lane=0;lane<4;++lane) s.vectors[12][lane]=lane==3 ? 0x41000000U : random_float(seed);
        tag(s,299,count,true);
        tag(s,500,1,false); tag(s,502,2,true);
    }
    if (entry==efu_entry) s.vectors[19]={random_word(seed)&1U,random_word(seed)&0xffffU,random_word(seed),random_word(seed)}; // ITOF0 then ESIN of x: 0 or 1
    result.name="entry "+std::to_string(entry)+" variant "+std::to_string(variant);
    return result;
}
gpu::JobStatus status(nt::vu::RunStatus run) {
    switch (run) {
    case nt::vu::RunStatus::complete: return gpu::JobStatus::complete;
    case nt::vu::RunStatus::unsupported_numeric: return gpu::JobStatus::unsupported_numeric;
    case nt::vu::RunStatus::unmapped_block: return gpu::JobStatus::unmapped_block;
    case nt::vu::RunStatus::budget_exhausted: return gpu::JobStatus::budget_exhausted;
    case nt::vu::RunStatus::unsupported_schedule: return gpu::JobStatus::unsupported_schedule;
    case nt::vu::RunStatus::unsupported_memory: return gpu::JobStatus::unsupported_memory;
    default: return gpu::JobStatus::unsupported_context;
    }
}
// The host loop of the CPU target (ee_vu_launch.hpp), with the kernel's
// per-job output capacity.
Outcome reference(const nt::vu::Program& program,const Job& job) {
    Outcome result;
    result.state=std::make_unique<nt::vu::State>(*job.state);
    auto& state=*result.state;
    for (;;) {
        const auto run=program.run(state,job.budget-result.blocks);
        result.blocks+=static_cast<std::uint32_t>(run.blocks);
        if (run.status!=nt::vu::RunStatus::graphics_pending) { result.status=status(run.status); return result; }
        std::vector<nt::graphics::Quadword> packet;
        try { packet=nt::graphics::collect_packet(state.data,state.graphics_address); }
        catch (const std::invalid_argument&) { result.status=gpu::JobStatus::invalid_packet; return result; }
        if (packet.size()>job.capacity-result.output.size()/4) { result.status=gpu::JobStatus::output_full; return result; }
        for (const auto& quadword:packet) result.output.insert(result.output.end(),quadword.begin(),quadword.end());
        state.graphics_pending=0;
    }
}
bool block_failure(gpu::JobStatus status) {
    return status==gpu::JobStatus::unsupported_numeric || status==gpu::JobStatus::unsupported_schedule ||
        status==gpu::JobStatus::unsupported_memory || status==gpu::JobStatus::unsupported_context;
}
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"Expected compiled VU program shader path");
        const auto shader=vulkan_compute::read_shader(argv[1]);
        const nt::vu::Program program(1,{nt_vu_gpu_fixture_blocks(),nt_vu_gpu_fixture_count()});
        std::vector<Job> jobs;
        for (const unsigned entry:{transform_entry,efu_entry,scheduled_entry})
            for (unsigned variant=0;variant<48;++variant) jobs.push_back(job(entry,0x9e3779b9U*(entry+1)+variant*7919U,variant));
        // Kernel exits other than completion.
        jobs.push_back(job(transform_entry,11,3)); jobs.back().budget=3; jobs.back().name="block budget";
        jobs.push_back(job(transform_entry,12,7)); jobs.back().state->next_pair=7; jobs.back().name="unmapped block";
        jobs.push_back(job(transform_entry,13,5)); jobs.back().capacity=4; jobs.back().name="output capacity";
        jobs.push_back(job(transform_entry,14,2)); tag(*jobs.back().state,299,2000,true); jobs.back().name="packet beyond ring";
        jobs.push_back(job(transform_entry,15,2)); tag(*jobs.back().state,502,2,false); jobs.back().name="packet without end";
        // A sine argument outside the supported range stops after the first block.
        jobs.push_back(job(efu_entry,16,1)); jobs.back().state->vectors[19][0]=60000; jobs.back().name="numeric stop";
        std::vector<Outcome> expected;
        for (const auto& job:jobs) expected.push_back(reference(program,job));

        const auto count=static_cast<std::uint32_t>(jobs.size());
        std::vector<std::uint32_t> control(1+count*gpu::job_words),states(count*gpu::state_words),output;
        control[0]=count;
        for (std::uint32_t n=0;n<count;++n) {
            auto* fields=control.data()+1+n*gpu::job_words;
            fields[gpu::job_state]=n*static_cast<std::uint32_t>(gpu::state_words);
            fields[gpu::job_output]=static_cast<std::uint32_t>(output.size());
            fields[gpu::job_output_capacity]=jobs[n].capacity;
            fields[gpu::job_budget]=jobs[n].budget;
            fields[gpu::job_status]=static_cast<std::uint32_t>(gpu::JobStatus::not_run);
            output.resize(output.size()+jobs[n].capacity*4,0xdeadbeefU);
            gpu::pack(*jobs[n].state,std::span<std::uint32_t,gpu::state_words>(states.data()+n*gpu::state_words,gpu::state_words));
        }
        vulkan_compute::Device device; device.initialize();
        const std::span<std::uint32_t> buffers[]{control,states,output};
        device.run(shader,buffers,(count+gpu::workgroup_size-1)/gpu::workgroup_size);

        unsigned completed=0,kicks=0;
        for (std::uint32_t n=0;n<count;++n) {
            const auto* fields=control.data()+1+n*gpu::job_words;
            const auto& want=expected[n];
            const auto got=static_cast<gpu::JobStatus>(fields[gpu::job_status]);
            const auto where=jobs[n].name+": ";
            if (got!=want.status) throw std::runtime_error(where+"GPU status "+std::to_string(fields[gpu::job_status])+
                ", CPU status "+std::to_string(static_cast<std::uint32_t>(want.status)));
            if (fields[gpu::job_blocks]!=want.blocks) throw std::runtime_error(where+"block counts differ");
            if (fields[gpu::job_output_quadwords]*4!=want.output.size() ||
                !std::equal(want.output.begin(),want.output.end(),output.begin()+fields[gpu::job_output]))
                throw std::runtime_error(where+"GIF output differs");
            if (block_failure(got)) continue; // GPU blocks are not transactions.
            nt::vu::State state;
            gpu::unpack(std::span<const std::uint32_t,gpu::state_words>(states.data()+n*gpu::state_words,gpu::state_words),state);
            if (!(state==*want.state)) throw std::runtime_error(where+"final VU state differs");
            completed+=got==gpu::JobStatus::complete;
            kicks+=static_cast<unsigned>(!want.output.empty());
        }
        check(completed>=120,"Too few fixture jobs completed to exercise the program");
        check(expected[count-6].status==gpu::JobStatus::budget_exhausted && expected[count-5].status==gpu::JobStatus::unmapped_block &&
              expected[count-4].status==gpu::JobStatus::output_full && expected[count-3].status==gpu::JobStatus::invalid_packet &&
              expected[count-2].status==gpu::JobStatus::invalid_packet && expected[count-1].status==gpu::JobStatus::unsupported_numeric,
              "Exit fixtures did not reach their exits");
        std::cout<<"Verified "<<count<<" VU1 jobs ("<<completed<<" complete, "<<kicks<<" with GIF output): "
                 <<"Vulkan kernel equals CPU blocks in status, blocks, state and packets.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
