#include <nt/vu_native.hpp>
#include <nt/vu_dispatch.hpp>
#include <nt/vu_binding.hpp>
#include <nt/ee_packet_setup.hpp>
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
#include <nt/ee_vector_owner.hpp>
#include <nt/vu_artifact.hpp>
#include <nt/vu_graphics.hpp>
#include <nt/vu_clip.hpp>
#include <nt/vu_quotient.hpp>
#include <nt/ee_platform.hpp>
#include <thread>
#include <iostream>
#include <cmath>
#include <stdexcept>

extern "C" std::uint64_t nt_ee_multiply_accumulate_finite(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" std::uint64_t nt_ee_add_finite(std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" std::uint64_t nt_ee_multiply_finite(std::uint32_t,std::uint32_t);
extern "C" std::uint64_t nt_vu_add_fast(std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" std::uint64_t nt_vu_multiply_fast(std::uint32_t,std::uint32_t);
extern "C" std::uint64_t nt_vu_multiply_accumulate_fast(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" nt::vu::Result nt_vu_generated(nt::vu::State*);
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" const nt::vu::Block* nt_vu_data_blocks();
extern "C" std::uint32_t nt_vu_data_count();
extern "C" const std::uint8_t* nt_vu_data_image();
extern "C" std::uint32_t nt_vu_data_size();
extern "C" const nt::vu::Block* nt_vu_shared_blocks();
extern "C" std::uint32_t nt_vu_shared_count();
extern "C" const std::uint8_t* nt_vu_shared_image();
extern "C" std::uint32_t nt_vu_shared_size();
extern "C" const nt::vu::Block* nt_vu_graphics_batch_blocks();
extern "C" std::uint32_t nt_vu_graphics_batch_count();
extern "C" const std::uint8_t* nt_vu_graphics_batch_image();
extern "C" std::uint32_t nt_vu_graphics_batch_size();
extern "C" nt::vu::Result nt_vu_branch(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_call(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_end_false(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_end_true(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_start(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_finish(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_early(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_elapsed(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_scheduled(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_program_blocks();
extern "C" std::uint32_t nt_vu_program_count();
extern "C" const std::uint8_t* nt_vu_program_image();
extern "C" std::uint32_t nt_vu_program_size();
extern "C" std::uint32_t nt_vu_program_destination();
extern "C" std::uint32_t nt_vu_program_unit();
extern "C" nt::vu::Result nt_vu_memory(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_memory1(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_integer_branch(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_geometry(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_0(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_3(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_6(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_8(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_12(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_22(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_26(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_29(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_52(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_55(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_83(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_p_86(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_0(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_3(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_8(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_10(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_14(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_18(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_graphics_blocks();
extern "C" std::uint32_t nt_vu_graphics_count();
extern "C" const nt::vu::Block* nt_vu_graphics_end_blocks();
extern "C" std::uint32_t nt_vu_graphics_end_count();
extern "C" nt::vu::Result nt_vu_clip_0(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_clip_4(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_clip_18(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_clip_blocks();
extern "C" std::uint32_t nt_vu_clip_count();
extern "C" const nt::vu::Block* nt_vu_continuation_blocks();
extern "C" std::uint32_t nt_vu_continuation_count();
extern "C" const std::uint8_t* nt_vu_continuation_image();
extern "C" std::uint32_t nt_vu_continuation_size();
extern "C" nt::vu::Result nt_vu_range(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_whole_blocks();
extern "C" std::uint32_t nt_vu_whole_count();
extern "C" const nt::vu::Block* nt_vu_mac_pipeline_blocks();
extern "C" std::uint32_t nt_vu_mac_pipeline_count();
namespace {
void test_clip_publication() {
    auto storage=std::make_unique<nt::vu::State>();
    const std::span blocks(nt_vu_clip_blocks(),nt_vu_clip_count());
    unsigned address=20;
    for (const unsigned count:{2U,3U,4U,5U,31U,32U,33U,64U}) {
        const auto block=std::find_if(blocks.begin(),blocks.end(),[&](const auto& b){return b.address==address;});
        if (block==blocks.end()) throw std::runtime_error("Missing clip publication fixture");
        for (unsigned selection=0;selection<20;++selection) for (const unsigned delay:{0U,4U,0xffffffffU}) {
            auto& state=*storage; state={};
            state.clip=0x123456; state.visible_clip=0x789abc;
            state.pending_clip={0x110001,0x220002,0x330003,0x440004};
            state.pending_clip_mask=selection<16 ? selection : 0x80000000U|(1U<<(selection-16));
            state.clip_read_delay=delay;
            auto expected_slots=state.pending_clip;
            auto expected_visible=state.visible_clip,expected_mask=state.pending_clip_mask;
            for (unsigned step=0;step<count;++step) {
                if (expected_mask) {
                    if (expected_mask&1) expected_visible=expected_slots[0];
                    for (unsigned i=0;i<3;++i) expected_slots[i]=expected_slots[i+1];
                    expected_mask>>=1;
                } else expected_visible=state.clip;
            }
            if (block->entry(&state)!=nt::vu::Result::transfer || state.clip!=0x123456 ||
                state.visible_clip!=expected_visible || state.pending_clip!=expected_slots ||
                state.pending_clip_mask!=expected_mask || state.clip_read_delay!=(delay>count ? delay-count : 0))
                throw std::runtime_error("Batched clip publication changed native boundary state: count="+std::to_string(count)+
                    " mask_case="+std::to_string(selection)+" visible="+std::to_string(state.visible_clip)+
                    " expected="+std::to_string(expected_visible)+" pending="+std::to_string(state.pending_clip_mask)+
                    " expected_pending="+std::to_string(expected_mask));
        }
        address+=count;
    }
}
void test_scalar_publication() {
    // The ADD/branch region has three pairs and no Q/P observers. Pending
    // scalar results must cross it exactly, including status bits independent
    // of the ADD's MAC flags and results that are still pending at the exit.
    auto storage=std::make_unique<nt::vu::State>();
    for (const auto q_delay:{0U,1U,2U,3U,4U,7U,0xffffffffU})
        for (const auto p_delay:{0U,1U,3U,4U,54U,0xffffffffU})
            for (const auto active:{0U,1U}) {
        auto& pending=*storage; pending={};
        pending.vectors[3].fill(0x3f800000); pending.vectors[4].fill(0x40000000);
        pending.status=pending.visible_status=0x410;
        pending.quotient=0x3f800000; pending.pending_quotient=0x40400000;
        pending.quotient_flags=0x20; pending.quotient_pending=active; pending.quotient_read_delay=q_delay;
        pending.scalar_p=0x40000000; pending.pending_p=0x40800000;
        pending.p_pending=active; pending.p_read_delay=p_delay;
        const bool q_due=q_delay && q_delay<=3,p_due=p_delay && p_delay<=3;
        if (nt_vu_branch(&pending)!=nt::vu::Result::transfer || pending.vectors[5][0]!=0x40400000 ||
            pending.quotient!=(q_due && active ? 0x40400000U : 0x3f800000U) ||
            pending.scalar_p!=(p_due && active ? 0x40800000U : 0x40000000U) ||
            pending.quotient_pending!=(q_due ? 0U : active) || pending.p_pending!=(p_due ? 0U : active) ||
            pending.quotient_read_delay!=(q_delay>3 ? q_delay-3 : 0U) ||
            pending.p_read_delay!=(p_delay>3 ? p_delay-3 : 0U) ||
            pending.status!=(q_due && active ? 0xc20U : 0x410U) || pending.visible_status!=pending.status)
            throw std::runtime_error("Scalar publication across compiled arithmetic region changed state or flags");
    }
}
void test_async_submission() {
    const auto image=std::span(nt_vu_graphics_batch_image(),nt_vu_graphics_batch_size());
    nt::vu::Binding binding(nt::vu::Program(1,{nt_vu_graphics_batch_blocks(),nt_vu_graphics_batch_count()}),0,image);
    if (!binding.upload(1,std::array{nt::vu::UploadView{0,image}})) throw std::runtime_error("Async fixture binding failed");
    nt::ee::NativeVuLaunch owner(binding); owner.enable_async();
    owner.state.integers[0]=0xffff; owner.state.data[1023]={0x8001,0x10000000,0,0};
    owner.state.vectors[0]={55,66,77,88};
    const auto before=owner.state;
    struct Sink { unsigned calls=0; bool accept=true; std::vector<nt::graphics::Packet> packets; } sink;
    owner.graphics={&sink,[](void* raw,std::span<const nt::graphics::Packet> packets) {
        auto& sink=*static_cast<Sink*>(raw); ++sink.calls;
        if (!sink.accept) return false;
        sink.packets.assign(packets.begin(),packets.end()); return true;
    }};
    nt::ee::NativePlatform platform;
    std::vector<std::uint8_t> ram(4096);
    nt::ee::Memory memory; memory.data=ram.data(); memory.size=ram.size(); platform.attach(memory);
    nt::ee::NativeVuSubmissions routing; routing.add(owner);
    routing.connect(platform.packet_setup); owner.connect_data(memory);
    const auto start=[&] {
        const std::array<std::uint32_t,8> words{0x01000101,0x6c010000,11,22,33,44,0x94000000,0};
        std::memcpy(ram.data()+256,words.data(),sizeof(words));
        auto& submit=platform.packet_submission;
        if (submit.write(memory,0x10009010,256)!=nt::ee::Stop::budget ||
            submit.write(memory,0x10009020,2)!=nt::ee::Stop::budget ||
            submit.write(memory,0x10009000,0x101)!=nt::ee::Stop::budget)
            throw std::runtime_error("Async compiled submission was not accepted");
    };
    start();
    // Compiled EE interrupt acknowledgement remains independent of an owned
    // VU job. Waiting here would hold the interrupt handler until rendering.
    platform.graphics.drawing_completed();
    nt::runtime::State acknowledgement;
    acknowledgement.pc=0x4f0020; acknowledgement.set(4,0x12001000); acknowledgement.set(8,2);
    if (nt_ee_generated_run(&acknowledgement,&memory,1)!=nt::ee::Stop::budget ||
        acknowledgement.executed!=1 || (platform.graphics.read(0x12001000).value&2) ||
        memory.io.write32(memory.io.context,0x12001010,0x7f00)!=nt::ee::Stop::budget ||
        platform.graphics.read(0x12001010).value!=0x7f00 ||
        memory.io.write32(memory.io.context,0x12001000,8)!=nt::ee::Stop::budget ||
        memory.io.write64(memory.io.context,0x12001000,0x200)!=nt::ee::Stop::native_wait ||
        memory.io.write64(memory.io.context,0x12001000,0x100)!=nt::ee::Stop::native_wait ||
        memory.io.write64(memory.io.context,0x12000070,0x14000a008cULL)!=nt::ee::Stop::budget ||
        platform.graphics.output().buffers[0]!=0x14000a008cULL)
        throw std::runtime_error("Pending VU job blocked display/interrupt controls or released flush/reset");
    const std::array<std::uint32_t,4> independent{0x01000101,0x20000000,0x12345678,0};
    const auto independent_bytes=std::span(reinterpret_cast<const std::uint8_t*>(independent.data()),sizeof(independent));
    if (platform.packet_setup.consume(0,independent_bytes,false)!=nt::ee::Stop::budget ||
        platform.packet_setup.read(0x10003870).value!=0x12345678 ||
        platform.packet_setup.write(0x10003870,0x87654321)!=nt::ee::Stop::budget ||
        platform.packet_setup.read(0x10003870).value!=0x87654321 ||
        platform.packet_setup.write(0x10003c70,0)!=nt::ee::Stop::native_wait ||
        platform.packet_setup.consume(1,independent_bytes,false)!=nt::ee::Stop::native_wait ||
        platform.packet_setup.consume(2,{},false)!=nt::ee::Stop::native_wait)
        throw std::runtime_error("Independent VU0 work stalled behind VU1 or conflicting work escaped ownership");
    if (!owner.pending() || !platform.packet_setup.working() || !platform.packet_setup.pending(1) ||
        owner.state!=before || sink.calls || platform.completions.status()&(1U<<1) ||
        !(platform.packet_submission.read(0x10009000).value&0x100) || platform.packet_setup.take_interrupts() ||
        !(platform.packet_setup.read(0x10003c00).value&4) || !*memory.vector_data[1].active)
        throw std::runtime_error("Async submission published completion or state early");
    std::fill(ram.begin(),ram.end(),0); // The queued job must own its input.
    const auto finish=[&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (platform.packet_setup.working()) {
            const auto result=platform.poll_submissions();
            if (result!=nt::ee::Stop::budget) return result;
            if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Async native job did not finish");
            std::this_thread::yield();
        }
        return nt::ee::Stop::budget;
    };
    if (finish()!=nt::ee::Stop::budget || sink.calls!=1 || sink.packets.size()!=2 ||
        sink.packets[0][1]!=std::array<std::uint32_t,4>{11,22,33,44} ||
        sink.packets[1][1]!=std::array<std::uint32_t,4>{55,66,77,88} ||
        platform.packet_setup.take_interrupts()!=2 || !platform.packet_setup.pending(1) ||
        platform.completions.status()&(1U<<1))
        throw std::runtime_error("Async publication lost snapshots or interrupt-stalled suffix");
    if (memory.io.write32(memory.io.context,0x10003c10,8)!=nt::ee::Stop::budget ||
        platform.packet_setup.pending(1) || !(platform.completions.status()&(1U<<1)) ||
        platform.packet_submission.read(0x10009000).value&0x100)
        throw std::runtime_error("Async suffix completion was not delivered exactly once");
    owner.state=before; sink.accept=false; platform.completions.acknowledge(2);
    start();
    if (finish()!=nt::ee::Stop::unsupported_vector || owner.state!=before ||
        platform.completions.status()&2 || sink.calls!=2)
        throw std::runtime_error("Async renderer rejection committed private state");
    if (platform.poll_submissions()!=nt::ee::Stop::unsupported_vector || owner.state!=before || sink.calls!=2)
        throw std::runtime_error("Failed asynchronous job later reported success");
}
nt::vu::Entry streamed_entry=nullptr;
std::atomic<unsigned> streamed_launches=0;
std::atomic<bool> streamed_second_entered=false;
std::shared_future<void> streamed_release;
nt::vu::Result gated_streamed_entry(nt::vu::State* state) {
    if (++streamed_launches==2) {
        streamed_second_entered=true;
        if (streamed_release.wait_for(std::chrono::seconds(5))!=std::future_status::ready)
            return nt::vu::Result::unsupported_schedule;
    }
    return streamed_entry(state);
}
void test_streamed_launches(bool fail_later) {
    const auto image=std::span(nt_vu_graphics_batch_image(),nt_vu_graphics_batch_size());
    const auto source=std::span(nt_vu_graphics_batch_blocks(),nt_vu_graphics_batch_count());
    std::vector<nt::vu::Block> blocks(source.begin(),source.end());
    for (auto& block:blocks) for (auto* link:{&block.target,&block.fallthrough})
        if (*link) *link=&blocks[static_cast<std::size_t>(*link-source.data())];
    streamed_entry=blocks.front().entry; blocks.front().entry=gated_streamed_entry;
    streamed_launches=0; streamed_second_entered=false;
    std::promise<void> release; streamed_release=release.get_future().share();
    nt::vu::Binding binding(nt::vu::Program(1,blocks),0,image);
    if (!binding.upload(1,std::array{nt::vu::UploadView{0,image}}))
        throw std::runtime_error("Streamed fixture binding failed");
    nt::ee::NativeVuLaunch owner(binding); owner.enable_async();
    owner.state.integers[0]=0xffff;
    owner.state.data[1023]={0x8001,0x10000000,0,0};
    owner.state.data[0]={11,22,33,44}; owner.state.vectors[0]={55,66,77,88};
    auto before=std::make_unique<nt::vu::State>(owner.state);
    std::vector<nt::graphics::Packet> packets;
    owner.graphics={&packets,[](void* raw,std::span<const nt::graphics::Packet> batch) {
        auto& output=*static_cast<std::vector<nt::graphics::Packet>*>(raw);
        output.insert(output.end(),batch.begin(),batch.end()); return true;
    }};
    std::array<nt::ee::NativeSubmissionStep,3> steps;
    steps[0].launch=nt::ee::NativeLaunchView{0,0,0}; steps[1].launch=steps[0].launch;
    steps[2].launch=nt::ee::NativeLaunchView{2047,0,0};
    if (owner.submit_sequence(1,std::span(steps).first(fail_later ? 3 : 2))!=nt::ee::Stop::budget)
        throw std::runtime_error("Streamed sequence was not accepted");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!streamed_second_entered && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const auto status=owner.poll();
    const bool early=streamed_second_entered && status==nt::ee::Stop::budget && owner.pending() &&
        owner.state==*before && packets.size()==2 && packets[0][1]==before->data[0] && packets[1][1]==before->vectors[0];
    release.set_value();
    if (!early) throw std::runtime_error("Completed graphics waited for unrelated later compiled work or published shared state early: entered="+std::to_string(streamed_second_entered.load())+" calls="+std::to_string(streamed_launches.load())+" status="+std::to_string(unsigned(status))+" packets="+std::to_string(packets.size())+" next="+std::to_string(owner.last_next_pair));
    auto result=nt::ee::Stop::budget;
    while (owner.pending() && std::chrono::steady_clock::now()<deadline) {
        result=owner.poll(); if (result!=nt::ee::Stop::budget) break;
        std::this_thread::yield();
    }
    if (owner.pending() || packets.size()!=4 || packets[1]!=packets[2] || packets[1]!=packets[3] ||
        result!=(fail_later ? nt::ee::Stop::unsupported_vector : nt::ee::Stop::budget) ||
        (fail_later && (owner.state!=*before || owner.poll()!=result)))
        throw std::runtime_error("Streamed completion/failure lost ordered completed packets or reported false success");
}
void test_memory_transactions() {
    nt::vu::State memory;
    memory.integers[1]=2; memory.integers[2]=1;
    memory.integers[7]=0x8001;
    memory.data[0]={9,9,9,9}; memory.vectors[0]={1,2,3,4}; memory.vectors[3]={5,6,7,8};
    memory.vectors[30]={0x12345678,0x87654321,0xabcdef01,0x10203040};
    memory.integers[14]=0x9876; memory.input_top=0x123; memory.input_itop=0x234;
    memory.random=0x3fabcdef; memory.graphics_address=0x345;
    const auto memory_before=memory;
    if (nt_vu_memory(&memory)!=nt::vu::Result::complete || memory.integers[0]!=0 || memory.integers[2]!=1 ||
        memory.vectors[1]!=std::array<std::uint32_t,4>{1,9,3,9} ||
        memory.vectors[2]!=std::array<std::uint32_t,4>{1,9,3,0x3f800000} ||
        memory.vectors[4]!=std::array<std::uint32_t,4>{5,6,7,8} ||
        memory.vectors[6]!=std::array<std::uint32_t,4>{5,6,7,0x8001} ||
        memory.data[0]!=std::array<std::uint32_t,4>{5,6,7,0x8001} || memory.data[2][0]!=1 ||
        memory.data[1]!=std::array<std::uint32_t,4>{0x8001,0,0x8001,0} || memory.integers[5]!=0x8001 || memory.integers[6]!=8 ||
        memory.vectors[30]!=memory_before.vectors[30] || memory.integers[14]!=memory_before.integers[14] ||
        memory.input_top!=memory_before.input_top || memory.input_itop!=memory_before.input_itop ||
        memory.random!=memory_before.random || memory.graphics_address!=memory_before.graphics_address)
        throw std::runtime_error("Native VU load/store addressing failed");
    auto mapped_memory=memory_before; mapped_memory.integers[1]=1024;
    const auto mapped_before=mapped_memory;
    if (nt_vu_memory(&mapped_memory)!=nt::vu::Result::unsupported_memory || mapped_memory!=mapped_before)
        throw std::runtime_error("Native VU mapped memory failure was not atomic");
    auto unit1=memory_before; unit1.integers[1]=1024; unit1.data[1023]={11,12,13,14};
    if (nt_vu_memory1(&unit1)!=nt::vu::Result::complete ||
        unit1.data[256]!=std::array<std::uint32_t,4>{5,6,7,8} ||
        unit1.vectors[6]!=std::array<std::uint32_t,4>{1,9,3,0x8001} ||
        unit1.data[0]!=std::array<std::uint32_t,4>{1,9,3,0x8001} ||
        unit1.vectors[5]!=std::array<std::uint32_t,4>{11,12,13,14})
        throw std::runtime_error("Native VU1 memory extent or signed offset failed");
}
}
int run_core_tests() {
    try {
        {
            if (nt_vu_whole_count()!=8) throw std::runtime_error("Whole VU entry coverage incomplete");
            nt::vu::Program program(0,{nt_vu_whole_blocks(),nt_vu_whole_count()});
            for (unsigned entry=0;entry<8;++entry) {
                nt::vu::State state; state.next_pair=entry; state.integers[1]=0;
                const auto result=program.run(state,8);
                const unsigned expected[]={0,3,3,2,1,0,1,1};
                if (state.integers[0]!=expected[entry] ||
                    result.status!=(entry>=6 ? nt::vu::RunStatus::unmapped_block : nt::vu::RunStatus::complete) ||
                    (entry>=6 && state.next_pair!=8))
                    throw std::runtime_error("Whole VU independent entry/delay/tail execution differs");
            }
            for (const unsigned target:{2U,3U,7U,511U}) {
                nt::vu::State state; state.next_pair=5; state.integers[1]=target;
                const auto result=program.run(state,8);
                const unsigned expected=target==2 ? 3 : target==3 ? 2 : target==7 ? 1 : 0;
                if (state.integers[0]!=expected || result.status!=(target<7 ? nt::vu::RunStatus::complete : nt::vu::RunStatus::unmapped_block) ||
                    (target>=7 && state.next_pair!=(target==7 ? 8 : target)))
                    throw std::runtime_error("Whole VU runtime indirect target was replaced or hidden");
            }
        }
        {
            nt::vu::Binding binding(nt::vu::Program(1,{nt_vu_continuation_blocks(),nt_vu_continuation_count()}),0,
                {nt_vu_continuation_image(),nt_vu_continuation_size()});
            if (!binding.upload(1,std::array{nt::vu::UploadView{0,{nt_vu_continuation_image(),nt_vu_continuation_size()}}}))
                throw std::runtime_error("Continuation fixture binding failed");
            nt::ee::NativeVuLaunch owner(binding);
            nt::ee::NativePacketSetup packets;
            owner.connect(packets);
            const auto submit=[&](std::initializer_list<std::uint32_t> words) {
                const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(words.begin()),words.size()*4);
                return packets.consume(1,bytes,false);
            };
            if (submit({0x17000000})!=nt::ee::Stop::unsupported_vector)
                throw std::runtime_error("Uninitialized continuation accepted");
            if (submit({0x03000020,0x02000010,0x04000007,0x14000000})!=nt::ee::Stop::budget ||
                owner.state.next_pair!=2 || owner.state.integers[0]!=1)
                throw std::runtime_error("Compiled E stop lost its continuation");
            owner.state.data[0]={0x8000,0,0,0}; // Original empty EOP packet.
            struct Consumer { bool accept=false; unsigned accepted=0; } consumer;
            owner.graphics.context=&consumer;
            owner.graphics.submit=[](void* raw,std::span<const nt::vu::GraphicsPacket> batch) {
                auto& c=*static_cast<Consumer*>(raw);
                if (!c.accept || batch.size()!=1) return false;
                ++c.accepted; return true;
            };
            const auto before=owner.state;
            const auto top=packets.read(0x10003ce0).value;
            if (submit({0x04000009,0x170007ff})!=nt::ee::Stop::unsupported_vector || owner.state!=before ||
                packets.read(0x10003ce0).value!=top || consumer.accepted)
                throw std::runtime_error("Rejected continuation committed state");
            consumer.accept=true;
            if (submit({0x04000009,0x170007ff})!=nt::ee::Stop::budget || owner.state.next_pair!=4 ||
                owner.state.integers[0]!=2 || owner.state.input_top!=0x30 || owner.state.input_itop!=9 || consumer.accepted!=1)
                throw std::runtime_error("Native continuation did not resume/update inputs exactly once");
            if (submit({0x17000000})!=nt::ee::Stop::budget || owner.state.next_pair!=6 ||
                owner.state.integers[0]!=3 || consumer.accepted!=1 || owner.state.graphics_completes)
                throw std::runtime_error("Continuation retained completed graphics boundary");
            const auto complete=owner.state;
            if (submit({0x01000101,0x6c010000,1,2,3,4,0x17000000})!=nt::ee::Stop::unsupported_vector ||
                owner.last_result.status!=nt::vu::RunStatus::unmapped_block || owner.state!=complete)
                throw std::runtime_error("Missing compiled continuation did not roll back upload");
            struct Completion { bool ready=false; unsigned calls=0; } completion;
            packets.graphics.context=&completion;
            packets.graphics.wait=[](void* raw) {
                auto& c=*static_cast<Completion*>(raw); ++c.calls; return c.ready;
            };
            if (submit({0x07000063,0x15000000})!=nt::ee::Stop::unsupported_system || owner.state!=complete ||
                packets.rejected_command()!=0x15 || packets.read(0x10003c30).value || completion.calls!=1)
                throw std::runtime_error("Failed graphics wait committed launch or configuration");
            completion.ready=true;
            if (submit({0x07000063,0x15000000})!=nt::ee::Stop::budget || owner.state.integers[0]!=4 ||
                packets.read(0x10003c30).value!=99 || completion.calls!=2)
                throw std::runtime_error("MSCALF retry did not run compiled entry exactly once");
            const auto after_wait=owner.state;
            if (submit({0x11000000,0x13000000,0x07000064})!=nt::ee::Stop::budget ||
                completion.calls!=3 || owner.state!=after_wait || packets.read(0x10003c30).value!=100)
                throw std::runtime_error("Native flush did not complete previous graphics");
            if (submit({0x11000000,0x7b000000})!=nt::ee::Stop::unsupported_system || completion.calls!=3)
                throw std::runtime_error("Invalid trailing command invoked graphics wait");
            if (submit({0x15000000,0x7b000000})!=nt::ee::Stop::unsupported_system ||
                completion.calls!=3 || owner.state!=after_wait)
                throw std::runtime_error("Invalid post-launch command published native work");
            packets.graphics.wait=nullptr;
            if (submit({0x11000000})!=nt::ee::Stop::unsupported_system)
                throw std::runtime_error("Missing native completion callback silently accepted");

        }
        nt::vu::State state;
        state.integers[0]=0xffff; state.integers[2]=2;
        state.vectors[0]={0x80000001,2,3,4};
        state.vectors[1]={0x80000000,0xbf800000,0xffc12345,0x7fffffff};
        state.vectors[4]={16,0xfffffff0,0x7fffffff,0x80000000};
        state.vectors[8]={0x80000000,0x3f800000,0x7f800001,0xbf800000};
        state.vectors[9]={0,0x3f000000,0x7f7fffff,0xc0000000};
        state.vectors[11]={111,222,333,444};
        state.vectors[13]={0x3f800000,0xbf800000,0x00800000,0x40000000};
        state.vectors[14]={0x40000000,0x3f800000,0x3f000000,0xc0000000};
        state.vectors[16]={99,99,99,99}; state.accumulator={88,88,88,88}; state.status=0x800;
        state.vectors[17]={99,99,99,99}; state.vectors[18]={99,99,99,99};
        const auto initial=state;
        if (nt_vu_generated(&state)!=nt::vu::Result::complete) throw std::runtime_error("Unexpected VU numeric rejection");
        if (state.integers[1]!=0xffff || state.integers[4]!=1 || state.immediate!=0x3f000000 ||
            state.vectors[0]!=std::array<std::uint32_t,4>{0,0x3f800000,0x7fc12345,0x7fffffff} ||
            state.vectors[2]!=std::array<std::uint32_t,4>{2,3,4,0x80000001} ||
            state.vectors[3]!=std::array<std::uint32_t,4>{0xffffffff,0xffffffff,0xffffffff,0xffffffff})
            throw std::runtime_error("Compiled VU pair semantics failed");
        if (state.vectors[5]!=std::array<std::uint32_t,4>{0x3f800000,0xbf800000,0x4cffffff,0xcd000000} ||
            state.vectors[6]!=std::array<std::uint32_t,4>{16,0xfffffff0,0x7fffff80,0x80000000} ||
            state.vectors[7]!=std::array<std::uint32_t,4>{0,0x3f800000,0x7f800001,0xbf800000} ||
            state.vectors[11]!=std::array<std::uint32_t,4>{0x80000000,222,0x3f000000,444} ||
            state.vectors[10]!=std::array<std::uint32_t,4>{0x12345678,0x3f800000,0x7f800001,0x12345678})
            throw std::runtime_error("Compiled VU conversions and comparisons failed");
        if (state.vectors[15]!=std::array<std::uint32_t,4>{0x40400000,0,0x3f000000,0} ||
            state.vectors[16]!=std::array<std::uint32_t,4>{99,0xc0400000,99,0} ||
            state.accumulator!=std::array<std::uint32_t,4>{0x40000000,88,0,88} || state.mac!=0x252 || state.status!=0x9c7)
            throw std::runtime_error("Compiled VU finite arithmetic flags failed");
        if (state.vectors[17]!=std::array<std::uint32_t,4>{0x40200000,0xbf800000,99,0xc0800000} ||
            state.vectors[18]!=std::array<std::uint32_t,4>{0xbfc00000,99,99,99})
        {
            for (const auto index:{17,18}) for (const auto value:state.vectors[index]) std::cerr<<std::hex<<value<<' ';
            std::cerr<<'\n';
            throw std::runtime_error("Compiled VU accumulator forms failed");
        }
        if (nt_ee_multiply_accumulate_finite(0xbf800000,0x3f800001,0x3f7ffffe,0)!=0xb3800000ULL ||
            nt_ee_multiply_accumulate_finite(0x3f800000,0x00800000,0x3f000000,0)!=(0x3f800000ULL|(5ULL<<36)))
            throw std::runtime_error("VU multiply/accumulate rounding boundary failed");
        auto underflow=initial;
        underflow.vectors[13][0]=0x00800000; underflow.vectors[14][0]=0x3f000000;
        if (nt_vu_generated(&underflow)!=nt::vu::Result::complete || underflow.vectors[17][0]!=0 ||
            underflow.vectors[18][0]!=0 || !(underflow.status&0x100))
            throw std::runtime_error("VU intermediate product underflow did not retain flags");
        for (const auto sign:{0U,0x80000000U}) {
            nt::vu::State product;
            product.vectors[0][0]=0x00800000U|sign;
            product.vectors[1][0]=0x3f000000;
            product.accumulator[0]=0x3f800000;
            const auto sticky=sign ? 0x1c0U : 0x140U;
            if (nt_vu_flags_14(&product)!=nt::vu::Result::transfer || product.integers[8] ||
                product.vectors[2][0]!=0x3f800000 || product.mac || product.status!=sticky)
                throw std::runtime_error("Product sticky flags published before their compiled boundary");
            if (nt_vu_flags_18(&product)!=nt::vu::Result::complete || product.integers[9]!=sticky ||
                product.visible_status!=sticky || product.visible_mac)
                throw std::runtime_error("Product sticky flags lost across compiled blocks");
        }
        for (const bool taken:{false,true}) {
            nt::vu::State branch; branch.integers[0]=5; branch.integers[1]=taken ? 5 : 6;
            if (nt_vu_branch(&branch)!=nt::vu::Result::transfer || branch.next_pair!=(taken ? 5U : 3U) ||
                branch.integers[0]!=6 || branch.integers[2]!=1 || branch.delayed_integer!=3 || branch.delayed_value!=0)
                throw std::runtime_error("Compiled VU branch/delay behavior failed");
        }
        test_scalar_publication();
        test_clip_publication();
        nt::vu::State call; call.integers[0]=7;
        if (nt_vu_call(&call)!=nt::vu::Result::transfer || call.next_pair!=7 || call.integers[0]!=102 || call.integers[2]!=102)
            throw std::runtime_error("Compiled VU indirect link alias failed");
        call={}; call.integers[0]=9; call.delayed_integer=1; call.delayed_value=511;
        if (nt_vu_call(&call)!=nt::vu::Result::transfer || call.next_pair!=9)
            throw std::runtime_error("Compiled VU indirect jump used stale conditional-branch input");
        {
            nt::vu::State range;
            range.vectors[0]={0x7fffffff,0xffffffff,0x7f800000,0};
            range.vectors[1]={0x40000000,0x40000000,0x3f800000,0x7fffffff};
            const std::array<std::uint32_t,4> expected{0x7fffffff,0xffffffff,0x7f800000,0};
            if (nt_vu_range(&range)!=nt::vu::Result::complete || range.vectors[2]!=expected ||
                range.vectors[3]!=expected ||
                range.vectors[4]!=std::array<std::uint32_t,4>{0xffffffff,0x7fffffff,0xff800000,0} ||
                range.mac!=0xc0a1 || range.status!=0x2cb)
                throw std::runtime_error("Compiled VU extended MUL/MADD or lane flags failed");
            // Overflow must take precedence even over an opposing maximum
            // accumulator. Exercise both product signs and add/subtract forms.
            for (const auto sign:{0U,0x80000000U}) {
                nt::vu::State saturated;
                saturated.vectors[0].fill(0x7f800000U|sign);
                saturated.vectors[1]={0x40000000,0x40800000,0x40000000,0x40800000};
                saturated.accumulator={0x7fffffff,0xffffffff,0,0x80000000};
                const auto product=0x7fffffffU|sign;
                if (nt_vu_range(&saturated)!=nt::vu::Result::complete ||
                    saturated.vectors[2]!=std::array<std::uint32_t,4>{product,product,product,product} ||
                    saturated.vectors[3]!=saturated.vectors[2] ||
                    saturated.vectors[4]!=std::array<std::uint32_t,4>{product^0x80000000U,product^0x80000000U,
                                                             product^0x80000000U,product^0x80000000U} ||
                    saturated.mac!=(sign ? 0xf000U : 0xf0f0U) ||
                    saturated.status!=(sign ? 0x288U : 0x28aU))
                    throw std::runtime_error("Compiled VU product overflow lost precedence over accumulator");
            }
        }
        // Exercise the arithmetic shortcuts through an LLVM-compiled ADD/branch,
        // including exponent separation on both sides of the fast-path boundary.
        for (const auto values:std::array<std::array<std::uint32_t,3>,18>{{
            {0,0x3f800000,0x3f800000},{0x80000001,0x80000000,0x80000000},
            {0x007fffff,0x80000001,0},{0x80000000,0x7fffffff,0x7fffffff},
            {0x3f800000,0x33000000,0x3f800000},{0x3f800000,0xb3000000,0x3f7fffff},
            {0x3f800000,0x33800000,0x3f800000},{0x3f800000,0xb3800000,0x3f7fffff},
            {0xbf800000,0x00800000,0xbf7fffff},{0xbf800000,0x80800000,0xbf800000},
            {0x7fffffff,0x00800000,0x7fffffff},{0x7f800000,0x80800000,0x7f7fffff},
            {0x3f800001,0xbf800000,0x34000000},{0xbf800001,0x3f800000,0xb4000000},
            {0x3f800000,0xbf7fffff,0x33800000},{0x00800001,0x80800000,0},
            {0x7fffffff,0xffffffff,0},{0x3fffffff,0x33800000,0x3fffffff}}}) {
            for (const bool swap:{false,true}) {
                auto sample=std::make_unique<nt::vu::State>();
                sample->vectors[3][0]=values[swap ? 1 : 0];
                sample->vectors[4][0]=values[swap ? 0 : 1];
                if (nt_vu_branch(sample.get())!=nt::vu::Result::transfer || sample->vectors[5][0]!=values[2])
                    throw std::runtime_error("Compiled VU zero/separated-exponent addition failed");
            }
        }
        nt::vu::State extended_branch; extended_branch.vectors[3][0]=0x7f800000;
        if (nt_vu_branch(&extended_branch)!=nt::vu::Result::transfer || extended_branch.vectors[5][0]!=0x7f800000 ||
            extended_branch.mac!=0 || extended_branch.status!=0)
            throw std::runtime_error("Compiled VU extended ADD/branch flags failed");
        const auto native=[] {
            std::array blocks{
                nt::vu::Block{5,nt_vu_end_true}, nt::vu::Block{0,nt_vu_branch},
                nt::vu::Block{3,nt_vu_end_false}, nt::vu::Block{100,nt_vu_call}};
            blocks[1].target=&blocks[0]; blocks[1].fallthrough=&blocks[2];
            return nt::vu::Program(0,blocks);
        }(); // Unsorted source descriptors need not outlive the native graph.
        for (const bool taken:{false,true}) {
            nt::vu::State routed; routed.integers[0]=5; routed.integers[1]=taken ? 5 : 6;
            const auto unchanged=routed;
            const auto empty=native.run(routed,0);
            if (empty.status!=nt::vu::RunStatus::budget_exhausted || empty.blocks!=0 || routed!=unchanged)
                throw std::runtime_error("Native VU zero budget mutated state");
            const auto paused=native.run(routed,1);
            if (paused.status!=nt::vu::RunStatus::budget_exhausted || paused.blocks!=1 ||
                routed.next_pair!=(taken ? 5U : 3U) || routed.integers[2]!=1)
                throw std::runtime_error("Native VU transfer budget failed");
            const auto resumed=native.run(routed,1);
            if (resumed.status!=nt::vu::RunStatus::complete || resumed.blocks!=1 || routed.integers[3]!=1)
                throw std::runtime_error("Native VU compiled continuation failed");
            routed=unchanged;
            const auto whole=native.run(routed,10);
            if (whole.status!=nt::vu::RunStatus::complete || whole.blocks!=2 || routed.integers[3]!=1)
                throw std::runtime_error("Native VU block routing failed");
        }
        nt::vu::State missing; missing.next_pair=100; missing.integers[0]=7;
        const auto stopped=native.run(missing,10);
        if (stopped.status!=nt::vu::RunStatus::unmapped_block || stopped.blocks!=1 || missing.next_pair!=7 ||
            missing.integers[0]!=102 || missing.integers[2]!=102)
            throw std::runtime_error("Native VU missing target did not preserve committed block");
        const auto missing_before=missing;
        if (native.run(missing,10).blocks!=0 || missing!=missing_before)
            throw std::runtime_error("Native VU missing entry executed code");
        for (const auto invalid:std::array{
                std::array{nt::vu::Block{0,nt_vu_branch},nt::vu::Block{0,nt_vu_call}},
                std::array{nt::vu::Block{0,nt_vu_branch},nt::vu::Block{512,nt_vu_call}},
                std::array{nt::vu::Block{0,nt_vu_branch},nt::vu::Block{1,nullptr}}}) {
            bool rejected=false;
            try { const nt::vu::Program invalid_program(0,invalid); }
            catch (const std::invalid_argument&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Invalid native VU registry accepted");
        }
        const std::array quotient_blocks{nt::vu::Block{0,nt_vu_q_start},nt::vu::Block{200,nt_vu_q_finish}};
        const nt::vu::Program quotient_program(0,quotient_blocks);
        nt::vu::State q;
        q.vectors[0][0]=0x41000000; q.vectors[1][0]=0x40800000; q.vectors[2][0]=0x40400000;
        q.quotient=0x3f800000;
        if (quotient_program.run(q,1).status!=nt::vu::RunStatus::budget_exhausted || q.next_pair!=200 ||
            q.quotient!=0x3f800000 || q.pending_quotient!=0x40000000 || q.quotient_pending!=1)
            throw std::runtime_error("Pending native quotient did not survive transfer");
        const auto pending_q=q;
        auto elapsed_q=q;
        if (nt_vu_q_elapsed(&elapsed_q)!=nt::vu::Result::complete || elapsed_q.vectors[3][0]!=0x40c00000 ||
            elapsed_q.quotient_pending || elapsed_q.quotient_read_delay)
            throw std::runtime_error("Scheduled Q completion was lost across native blocks");
        auto scheduled_q=q; scheduled_q.quotient_pending=0; scheduled_q.quotient_read_delay=0;
        if (nt_vu_q_scheduled(&scheduled_q)!=nt::vu::Result::complete ||
            scheduled_q.vectors[3][0]!=0x40c00000 || scheduled_q.vectors[4][0]!=0x40c00000 ||
            scheduled_q.vectors[5][0]!=0x41400000 || scheduled_q.quotient_pending || scheduled_q.quotient_read_delay)
            throw std::runtime_error("Scheduled DIV/SQRT/RSQRT did not publish Q without WAITQ");
        auto early_q=q;
        if (nt_vu_q_early(&early_q)!=nt::vu::Result::complete || early_q.vectors[3][0]!=0x40400000 ||
            early_q.quotient!=0x40000000 || early_q.quotient_pending)
            throw std::runtime_error("Early Q read did not retain the published value before E drains the result");
        if (quotient_program.run(q,1).status!=nt::vu::RunStatus::complete || q.quotient!=0x40800000 ||
            q.quotient_pending || q.vectors[3][0]!=0x40c00000 || q.vectors[4][0]!=0x40c00000 || q.vectors[5][0]!=0x41400000)
            throw std::runtime_error("Native synchronized quotient operations failed");
        auto invalid_q=pending_q; invalid_q.vectors[1][0]=0x7f800000;
        const auto invalid_q_before=invalid_q;
        if (nt_vu_q_finish(&invalid_q)!=nt::vu::Result::unsupported_numeric || invalid_q!=invalid_q_before)
            throw std::runtime_error("Native Q numeric failure partially committed");
        auto negative_q=pending_q; negative_q.vectors[1][0]=0xc0800000;
        if (nt_vu_q_finish(&negative_q)!=nt::vu::Result::complete || negative_q.quotient!=0x40800000 ||
            (negative_q.status&0xc30)!=0x410)
            throw std::runtime_error("Native Q invalid and sticky status failed");
        const auto div_zero=nt::numeric::vu_divide_finite(0x3f800000,0x80000000);
        const auto div_invalid=nt::numeric::vu_divide_finite(0,0);
        const auto root_zero=nt::numeric::vu_square_root_finite(0x80000000);
        const auto reciprocal_zero=nt::numeric::vu_reciprocal_square_root_finite(0,0x80000000);
        if (div_zero.bits!=0xff7fffff || div_zero.flags!=0x20 || div_invalid.flags!=0x10 ||
            root_zero.bits!=0 || root_zero.flags!=0 || reciprocal_zero.bits!=0x80000000 || reciprocal_zero.flags!=0x30)
            throw std::runtime_error("VU quotient exceptional results failed");
        if (nt_vu_program_count()!=3) throw std::runtime_error("Incorrect generated VU block count");
        const nt::vu::Program generated_program(0,{nt_vu_program_blocks(),nt_vu_program_count()});
        for (const bool taken:{false,true}) {
            nt::vu::State generated; generated.integers[0]=1; generated.integers[1]=taken ? 1 : 2;
            const auto execution=generated_program.run(generated,10);
            if (execution.status!=nt::vu::RunStatus::complete || execution.blocks!=2 || generated.integers[2]!=1 ||
                generated.integers[3]!=(taken ? 0 : 1) || generated.integers[4]!=(taken ? 1 : 0))
                throw std::runtime_error("Generated VU program routing failed");
        }
        test_memory_transactions();
        const std::span<const std::uint8_t> identity{nt_vu_program_image(),nt_vu_program_size()};
        nt::vu::Binding binding(generated_program,nt_vu_program_destination(),identity);
        nt::vu::State launched; launched.integers[0]=launched.integers[1]=1;
        const auto not_launched=launched;
        if (binding.launch(launched,0,10).status!=nt::vu::RunStatus::unbound_program || launched!=not_launched)
            throw std::runtime_error("Unbound VU program executed");
        const std::array first_upload{nt::vu::UploadView{0,identity.first(16)}};
        if (!binding.upload(nt_vu_program_unit(),first_upload) || binding.ready())
            throw std::runtime_error("Partial VU upload bound a program");
        nt::ee::NativePacketSetup packets;
        nt::ee::bind_uploads(packets,binding);
        std::vector<std::uint8_t> upload_packet;
        const auto packet_word=[&](std::uint32_t word) {
            for (unsigned byte=0;byte<4;++byte) upload_packet.push_back(static_cast<std::uint8_t>(word>>(byte*8)));
        };
        packet_word(0x07000063); packet_word(0x4a040002);
        upload_packet.insert(upload_packet.end(),identity.begin()+16,identity.end());
        auto wrong_packet=upload_packet; wrong_packet.back()^=1;
        if (packets.consume(0,wrong_packet,false)!=nt::ee::Stop::unsupported_vector || binding.ready() || packets.read(0x10003830).value)
            throw std::runtime_error("Wrong VU content changed binding or packet configuration");
        auto trailing_packet=upload_packet; trailing_packet.insert(trailing_packet.end(),{0,0,0,0x7b});
        if (packets.consume(0,trailing_packet,false)!=nt::ee::Stop::unsupported_system || binding.ready())
            throw std::runtime_error("Unsupported trailing command partially bound VU code");
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector || binding.ready())
            throw std::runtime_error("Wrong VU unit accepted binding");
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::budget || !binding.ready() || packets.read(0x10003830).value!=99)
            throw std::runtime_error("Matching VU upload did not bind atomically");
        if (binding.launch(launched,0,10).status!=nt::vu::RunStatus::complete || launched.integers[4]!=1)
            throw std::runtime_error("Bound native VU launch failed");
        binding.reset();
        if (binding.ready() || binding.resume(launched,10).status!=nt::vu::RunStatus::unbound_program)
            throw std::runtime_error("Reset VU binding retained executable coverage");
        auto binding1=nt::vu::linked_binding1();
        nt::ee::NativeVuUploads uploads;
        uploads.add(binding);
        uploads.add(binding1);
        bool duplicate=false;
        try { uploads.add(binding1); }
        catch (const std::invalid_argument&) { duplicate=true; }
        if (!duplicate) throw std::runtime_error("Duplicate VU binding accepted");
        uploads.connect(packets);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget || binding1.ready() || binding.ready())
            throw std::runtime_error("Partial routed upload changed another unit");
        upload_packet.clear();
        packet_word(0x4a020000);
        upload_packet.insert(upload_packet.end(),identity.begin(),identity.begin()+16);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget || !binding1.ready() || binding.ready())
            throw std::runtime_error("VU1 upload coverage was not independent");
        launched={}; launched.integers[0]=launched.integers[1]=1;
        if (binding1.launch(launched,0,10).status!=nt::vu::RunStatus::complete || launched.integers[4]!=1)
            throw std::runtime_error("Routed VU1 artifact did not execute native code");
        {
            auto switched=binding1;
            const auto* catalog=nt_vu_program1_artifacts_v3();
            if (nt_vu_program1_artifact_count()!=2) throw std::runtime_error("VU catalog did not export both images");
            const std::span alternative(catalog[1].image,catalog[1].image_size);
            const std::array replacement{nt::vu::UploadView{4,alternative.subspan(32,8)}};
            if (!switched.upload(1,replacement)) throw std::runtime_error("Known overlapping VU image was rejected");
            const auto before_resume=launched;
            if (switched.resume(launched,10).status!=nt::vu::RunStatus::unbound_program || launched!=before_resume)
                throw std::runtime_error("Overwritten native VU module retained its active continuation");
            nt::vu::State changed; changed.integers[0]=changed.integers[1]=1;
            if (switched.launch(changed,0,10).status!=nt::vu::RunStatus::complete || changed.integers[4]!=2)
                throw std::runtime_error("Overlapping upload did not select the newly compiled image");
            if (!switched.upload(1,std::array{nt::vu::UploadView{4,identity.subspan(32,8)}}))
                throw std::runtime_error("Original VU image could not be restored");
            changed={}; changed.integers[0]=changed.integers[1]=1;
            if (switched.launch(changed,0,10).status!=nt::vu::RunStatus::complete || changed.integers[4]!=1)
                throw std::runtime_error("Stale compiled VU image survived overwrite");
        }
        if (packets.consume(0,wrong_packet,false)!=nt::ee::Stop::unsupported_vector || binding.ready() || !binding1.ready())
            throw std::runtime_error("Rejected VU0 upload affected VU1 coverage");
        if (!binding.upload(0,std::array{nt::vu::UploadView{0,identity}}) || !binding.ready() || !binding1.ready())
            throw std::runtime_error("Both native VU units could not be bound together");
        nt::ee::NativeVuLaunch launch_owner(binding1);
        {
            auto combined_binding=nt::vu::linked_binding1();
            nt::ee::NativeVuLaunch combined_owner(combined_binding);
            combined_owner.state.integers[0]=combined_owner.state.integers[1]=1;
            nt::ee::NativePacketSetup combined_packets;
            nt::ee::bind_uploads(combined_packets,combined_binding);
            nt::ee::NativeVuSubmissions combined_submissions;
            combined_submissions.add(combined_owner); combined_submissions.connect(combined_packets);
            bool accept_drawing=false;
            const auto drawing_consumer=+[](void* raw,std::span<const nt::graphics::Packet> batch) {
                return *static_cast<bool*>(raw) && batch.size()==1 && batch[0]==nt::graphics::Packet{{0x8000,0,0,0}};
            };
            combined_packets.graphics={&accept_drawing,drawing_consumer};
            combined_owner.graphics={&accept_drawing,drawing_consumer};
            upload_packet.clear(); packet_word(0x0700002a); packet_word(0x01000101);
            packet_word(0x6c010000); for (unsigned lane=0;lane<4;++lane) packet_word(lane+17);
            packet_word(0x4a000000U|(static_cast<unsigned>(identity.size()/8)<<16));
            const auto code_offset=upload_packet.size();
            upload_packet.insert(upload_packet.end(),identity.begin(),identity.end());
            packet_word(0x50000001); packet_word(0x8000); packet_word(0); packet_word(0); packet_word(0);
            packet_word(0x14000001); // Missing native entry must roll back binding and data too.
            const auto before=combined_owner.state;
            if (combined_packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
                combined_binding.ready() || combined_owner.state!=before || combined_packets.read(0x10003c30).value)
                throw std::runtime_error("Combined upload/launch failure partially committed state");
            upload_packet[upload_packet.size()-4]=0;
            auto wrong=upload_packet; wrong[code_offset]^=1;
            if (combined_packets.consume(1,wrong,false)!=nt::ee::Stop::unsupported_vector ||
                combined_binding.ready() || combined_owner.state!=before)
                throw std::runtime_error("Combined launch executed mismatched uploaded code");
            auto trailing=upload_packet; trailing.insert(trailing.end(),{0,0,0,0x7b});
            if (combined_packets.consume(1,trailing,false)!=nt::ee::Stop::unsupported_system || combined_binding.ready())
                throw std::runtime_error("Trailing command committed combined upload");
            if (combined_packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
                combined_binding.ready() || combined_owner.state!=before)
                throw std::runtime_error("Rejected DIRECT prefix published vector state or binding");
            accept_drawing=true;
            if (combined_packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
                !combined_binding.ready() || combined_owner.state.integers[4]!=1 ||
                combined_owner.state.data[0]!=std::array<std::uint32_t,4>{17,18,19,20} ||
                combined_packets.read(0x10003c30).value!=42)
                throw std::runtime_error("Combined upload/data/launch did not execute compiled VU code");
            const auto per_launch=combined_owner.last_result.blocks;
            std::array<nt::ee::NativeSubmissionStep,2> sequence;
            for (auto& step:sequence) step.launch=nt::ee::NativeLaunchView{0,0,0};
            nt::ee::NativeVuLaunch bounded(combined_binding,per_launch,nullptr,per_launch*2);
            bounded.state=combined_owner.state;
            if (bounded.submit_sequence(1,sequence)!=nt::ee::Stop::budget || bounded.last_sequence.launches!=2 ||
                bounded.last_sequence.blocks!=per_launch*2 || bounded.last_sequence.largest_launch!=per_launch)
                throw std::runtime_error("Sequence incorrectly spent its per-launch budget across separate calls");
            nt::ee::NativeVuLaunch insufficient(combined_binding,per_launch,nullptr,per_launch*2-1);
            insufficient.state=combined_owner.state;
            const auto unchanged_sequence=insufficient.state;
            if (insufficient.submit_sequence(1,sequence)!=nt::ee::Stop::unsupported_vector ||
                insufficient.last_result.status!=nt::vu::RunStatus::budget_exhausted || insufficient.state!=unchanged_sequence)
                throw std::runtime_error("Aggregate compiled-work budget failed to roll back the sequence");
        }
        launch_owner.connect(packets);
        launch_owner.state.integers[0]=launch_owner.state.integers[1]=1;
        upload_packet.clear(); packet_word(0x03000020); packet_word(0x02000010); packet_word(0x04000009);
        packet_word(0x07000007); packet_word(0x14000000); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            launch_owner.state.integers[4]!=1 || packets.read(0x10003c30).value!=7)
            throw std::runtime_error("Packet launch did not execute compiled VU1 code");
        if (launch_owner.state.input_top!=32 || launch_owner.state.input_itop!=9 ||
            launch_owner.state.input_valid!=3 || packets.read(0x10003cc0).value!=48 ||
            !(packets.read(0x10003c00).value&0x80))
            throw std::runtime_error("Native launch inputs or buffer selection failed");
        if (packets.read(0x10003c60).value!=0 || packets.read(0x10003c80).value!=0 ||
            packets.read(0x10003c90).value!=9 || packets.read(0x10003ca0).value!=32 ||
            packets.read(0x10003cb0).value!=16 || packets.read(0x10003cd0).value!=9 ||
            packets.read(0x10003ce0).value!=32)
            throw std::runtime_error("VIF command metadata does not match the public NUM/CODE/ITOP layout");
        upload_packet.clear(); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            launch_owner.state.input_top!=48 || packets.read(0x10003cc0).value!=32 ||
            (packets.read(0x10003c00).value&0x80))
            throw std::runtime_error("Second native launch did not alternate buffers");
        auto completed_launch=launch_owner.state;
        packet_word(0x7b000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || launch_owner.state!=completed_launch)
            throw std::runtime_error("Trailing command executed a partial VU launch");
        upload_packet.clear(); packet_word(0x14000000); packet_word(0x0100ffff);
        packet_word(0x6c010000); for (unsigned lane=0;lane<4;++lane) packet_word(90+lane);
        packet_word(0x14000006); // Outside both complete six-pair artifacts.
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            launch_owner.state!=completed_launch || packets.read(0x10003cc0).value!=32)
            throw std::runtime_error("Failed second native launch committed the first launch or data");
        upload_packet[upload_packet.size()-4]=0;
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            launch_owner.state.input_top!=48 || packets.read(0x10003cc0).value!=32 ||
            launch_owner.state.data[0]!=std::array<std::uint32_t,4>{90,91,92,93})
            throw std::runtime_error("Ordered native launches lost buffer selection or intervening data");
        completed_launch=launch_owner.state;
        upload_packet.clear(); packet_word(0x07000008); packet_word(0x14000006);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            launch_owner.last_result.status!=nt::vu::RunStatus::unmapped_block ||
            launch_owner.state!=completed_launch || packets.read(0x10003c30).value!=7)
            throw std::runtime_error("Unmapped launch changed packet or execution state");
        nt::ee::NativeVuLaunch limited_launch(binding1,1);
        limited_launch.connect(packets);
        const auto before_limited=limited_launch.state;
        upload_packet.clear(); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            limited_launch.last_result.status!=nt::vu::RunStatus::budget_exhausted || limited_launch.state!=before_limited)
            throw std::runtime_error("Launch budget exhaustion committed partial execution");
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::unsupported_vector || limited_launch.state!=before_limited)
            throw std::runtime_error("VU0 launch bypassed shared state ownership");
        nt::vu::Binding data_binding(nt::vu::Program(1,{nt_vu_data_blocks(),nt_vu_data_count()}),0,
            {nt_vu_data_image(),nt_vu_data_size()});
        if (!data_binding.upload(1,std::array{nt::vu::UploadView{0,{nt_vu_data_image(),nt_vu_data_size()}}}))
            throw std::runtime_error("Synthetic data module did not bind");
        nt::ee::NativeVuLaunch data_owner(data_binding);
        for (std::size_t q=0;q<data_owner.state.data.size();++q)
            for (unsigned lane=0;lane<4;++lane) data_owner.state.data[q][lane]=0x12340000U+std::uint32_t(q*4+lane);
        auto expected_data=data_owner.state.data;
        expected_data[1023]={1,2,3,4}; expected_data[0]={5,6,7,8};
        data_owner.connect(packets);
        upload_packet.clear(); packet_word(0x6c0203ff);
        for (unsigned value=1;value<=8;++value) packet_word(value);
        packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data!=expected_data ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{5,6,7,8})
            throw std::runtime_error("Compiled VU did not read wrapped V4-32 upload");
        const auto uploaded_state=data_owner.state;
        upload_packet.clear(); packet_word(0x6c010000);
        for (unsigned value=9;value<=12;++value) packet_word(value);
        const auto data_packet=upload_packet;
        packet_word(0x14000001);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector || data_owner.state!=uploaded_state)
            throw std::runtime_error("Failed launch committed data upload");
        upload_packet=data_packet; upload_packet.resize(upload_packet.size()-4);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state || data_owner.state!=uploaded_state)
            throw std::runtime_error("Truncated upload committed data");
        upload_packet=data_packet; packet_word(0x7b000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || data_owner.state!=uploaded_state)
            throw std::runtime_error("Unsupported trailing command committed data");
        upload_packet.clear(); packet_word(0x03000040); packet_word(0x02000000); packet_word(0x6c018003);
        for (unsigned value=9;value<=12;++value) packet_word(value);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[67]!=std::array<std::uint32_t,4>{9,10,11,12})
            throw std::runtime_error("TOPS-relative upload destination failed");
        upload_packet.clear(); packet_word(0x01000100);
        upload_packet.insert(upload_packet.end(),data_packet.begin(),data_packet.end());
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector || packets.read(0x10003c40).value!=0xffff)
            throw std::runtime_error("Unsupported cycle mode committed configuration");
        const auto unpack_and_launch=[&](std::uint32_t code,std::initializer_list<std::uint32_t> payload,
                                        std::array<std::uint32_t,4> expected,unsigned mode=0) {
            upload_packet.clear(); packet_word(0x05000000U|mode); packet_word(code);
            for (const auto word:payload) packet_word(word);
            packet_word(0x14000000);
            if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
                data_owner.state.vectors[0]!=expected || data_owner.state.data[0]!=expected)
                throw std::runtime_error("Vector data upload failed native consumption");
        };
        unpack_and_launch(0x6d010000,{0xffff0000,0x80007fff},{0,0xffffffff,0x7fff,0xffff8000});
        unpack_and_launch(0x6d014000,{0xffff0000,0x80007fff},{0,0xffff,0x7fff,0x8000});
        unpack_and_launch(0x6e010000,{0x807fff00},{0,0xffffffff,0x7f,0xffffff80});
        unpack_and_launch(0x6e014000,{0x807fff00},{0,0xff,0x7f,0x80});
        // High halfword is transport padding, deliberately not zero.
        unpack_and_launch(0x6f010000,{0xdeadfc1f},{248,0,248,128},3);
        unpack_and_launch(0x6f014000,{0xffff03e0},{0,248,0,0},2);
        unpack_and_launch(0x60010000,{0x89abcdef},{0x89abcdef,0x89abcdef,0x89abcdef,0x89abcdef});
        unpack_and_launch(0x61010000,{0xdead8000},{0xffff8000,0xffff8000,0xffff8000,0xffff8000});
        unpack_and_launch(0x61014000,{0xdead8000},{0x8000,0x8000,0x8000,0x8000});
        unpack_and_launch(0x62010000,{0xdeadbe80},{0xffffff80,0xffffff80,0xffffff80,0xffffff80});
        unpack_and_launch(0x62014000,{0xdeadbe80},{128,128,128,128});
        unpack_and_launch(0x64010000,{0x89abcdef,0x01234567},{0x89abcdef,0x01234567,0x89abcdef,0x01234567});
        unpack_and_launch(0x65010000,{0x80007fff},{0x7fff,0xffff8000,0x7fff,0xffff8000});
        unpack_and_launch(0x65014000,{0x80007fff},{0x7fff,0x8000,0x7fff,0x8000});
        unpack_and_launch(0x66010000,{0xdead807f},{127,0xffffff80,127,0xffffff80});
        unpack_and_launch(0x66014000,{0xdead807f},{127,128,127,128});
        // Three V2-8 vectors cross the destination end; two bytes are padding.
        upload_packet.clear(); packet_word(0x660343ff); packet_word(0x04030201); packet_word(0xdead0605);
        packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1023]!=std::array<std::uint32_t,4>{1,2,1,2} ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{3,4,3,4} ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{5,6,5,6})
            throw std::runtime_error("V2 packed transport padding or destination wrap failed");
        upload_packet.clear(); packet_word(0x62004000);
        for (unsigned i=0;i<64;++i) packet_word((i*4)|((i*4+1)<<8)|((i*4+2)<<16)|((i*4+3)<<24));
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[255]!=std::array<std::uint32_t,4>{255,255,255,255})
            throw std::runtime_error("Scalar encoded-zero count did not consume 256 input bytes");
        const auto before_narrow_failure=data_owner.state;
        upload_packet.clear(); packet_word(0x64010000); packet_word(1);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state || data_owner.state!=before_narrow_failure)
            throw std::runtime_error("Truncated V2 upload committed data");
        for (const unsigned command:{0x63U,0x67U,0x6bU,0x73U,0x77U,0x7bU}) {
            upload_packet.clear(); packet_word((command<<24)|0x10000); packet_word(0);
            if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || data_owner.state!=before_narrow_failure)
                throw std::runtime_error("Unsupported shape or masked upload used approximate semantics");
        }
        const auto before_bad_format=data_owner.state;
        upload_packet.clear(); packet_word(0x7b010000); packet_word(0); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || data_owner.state!=before_bad_format)
            throw std::runtime_error("Invalid unpack silently used supported semantics");
        upload_packet.clear(); packet_word(0x05000000); packet_word(0x6d010000); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state || data_owner.state!=before_bad_format)
            throw std::runtime_error("Truncated packed upload changed native state");
        upload_packet.clear(); packet_word(0x05000000); packet_word(0x6e004000);
        for (unsigned vector=0;vector<256;++vector) packet_word(0x01010101U*vector);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[255]!=std::array<std::uint32_t,4>{255,255,255,255})
            throw std::runtime_error("Zero encoded count did not upload 256 packed vectors");
        // Masks select data, row, column or preserved destination lanes.
        const auto mask_setup=[&](unsigned cycle,std::uint32_t selectors) {
            upload_packet.clear(); packet_word(0x01000000|cycle|(cycle<<8));
            packet_word(0x30000000); for (unsigned v:{10U,20U,30U,40U}) packet_word(v);
            packet_word(0x31000000); for (unsigned v:{100U,200U,300U,400U}) packet_word(v);
            packet_word(0x20000000); packet_word(selectors);
        };
        // Zero CL supplies no input bytes. Masked row fill still writes data,
        // wraps local storage and can immediately feed compiled VU execution.
        for (const unsigned wl:{1U,4U,0U}) {
            mask_setup(1,0x55555555); packet_word(0x01000000|(wl<<8));
            packet_word(0x05000002); packet_word(0x7c0903fe); packet_word(0x14000000);
            if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
                data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{10,20,30,40} ||
                data_owner.state.data[1022]!=data_owner.state.vectors[0] ||
                data_owner.state.data[6]!=data_owner.state.vectors[0] || packets.read(0x10003d00).value!=10)
                throw std::runtime_error("Zero-input row fill consumed commands as payload or lost compiled output");
        }
        mask_setup(1,0x55555555); packet_word(0x01000000); packet_word(0x7c000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[255]!=std::array<std::uint32_t,4>{10,20,30,40})
            throw std::runtime_error("Zero-encoded WL/NUM did not fill 256 outputs without input");
        mask_setup(1,0x55555500); packet_word(0x01000001); packet_word(0x05000000);
        packet_word(0x70040000); packet_word(7); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{7,7,7,7} ||
            data_owner.state.data[3]!=std::array<std::uint32_t,4>{10,20,30,40})
            throw std::runtime_error("Zero-encoded WL lost its one-input fill cycle");
        for (unsigned i=0;i<6;++i) data_owner.state.data[i]={90,91,92,93};
        mask_setup(2,0x00001be4); // First group D,R,C,P; second P,C,R,D.
        packet_word(0x72040000); packet_word(0x04030201); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{1,20,100,93} ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{90,200,30,2} ||
            data_owner.state.data[2]!=std::array<std::uint32_t,4>{3,20,100,93})
            throw std::runtime_error("Masked scalar selection or cycle restart failed");
        mask_setup(255,0xaaaaaaaa); // Column selection saturates at column three.
        packet_word(0x76060000); packet_word(0); packet_word(0); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[0]!=std::array<std::uint32_t,4>{100,100,100,100} ||
            data_owner.state.data[4]!=std::array<std::uint32_t,4>{400,400,400,400})
            throw std::runtime_error("Masked V2 column saturation failed");
        mask_setup(255,0xe4); packet_word(0x7f010000); packet_word(0xfffffc1f); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{248,20,100,100})
            throw std::runtime_error("Masked packed-colour conversion failed");
        const auto before_mask_failure=data_owner.state;
        const auto old_mask=packets.read(0x10003c70).value;
        mask_setup(1,0xffffffff); packet_word(0x7d010000); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state ||
            data_owner.state!=before_mask_failure || packets.read(0x10003c70).value!=old_mask)
            throw std::runtime_error("Truncated masked upload published data or configuration");
        // A skipped slot is preserved while masked row addition wraps to 32 bits.
        data_owner.state.data[1023]={11,12,13,14};
        upload_packet.clear(); packet_word(0x01000203); packet_word(0x05000001);
        packet_word(0x30000000); packet_word(0xffffffff); packet_word(10); packet_word(20); packet_word(30);
        packet_word(0x600403fd); for (unsigned v:{1U,2U,3U,4U}) packet_word(v);
        packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1021]!=std::array<std::uint32_t,4>{0,11,21,31} ||
            data_owner.state.data[1022]!=std::array<std::uint32_t,4>{1,12,22,32} ||
            data_owner.state.data[1023]!=std::array<std::uint32_t,4>{11,12,13,14} ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{2,13,23,33} ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{3,14,24,34} ||
            packets.read(0x10003d00).value!=0xffffffff)
            throw std::runtime_error("Skipped-cycle row addition changed gaps, wrap or row state");
        mask_setup(1,0xe4); packet_word(0x05000001); packet_word(0x72010000); packet_word(5); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{15,20,100,33})
            throw std::runtime_error("Row addition applied to masked replacement/preserved lanes");
        const auto before_row_failure=data_owner.state;
        const auto prior_cycle=packets.read(0x10003c40).value;
        upload_packet.clear(); packet_word(0x01000104); packet_word(0x05000002); packet_word(0x60010000); packet_word(99); packet_word(0x7b000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system ||
            data_owner.state!=before_row_failure || packets.read(0x10003c40).value!=prior_cycle)
            throw std::runtime_error("Invalid trailing command committed row updates");
        mask_setup(255,0);
        packet_word(0x05000002); packet_word(0x60020000); packet_word(1); packet_word(2);
        packet_word(0x05000003); packet_word(0x64010002); packet_word(9); packet_word(8);
        packet_word(0x05000002); packet_word(0x60010003); packet_word(1); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{11,21,31,41} ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{13,23,33,43} ||
            data_owner.state.data[2]!=std::array<std::uint32_t,4>{9,8,9,8} ||
            data_owner.state.data[3]!=std::array<std::uint32_t,4>{10,9,10,9} ||
            packets.read(0x10003d00).value!=10 || packets.read(0x10003d10).value!=9)
            throw std::runtime_error("Mutable row modes did not compose across uploads in one native batch");
        mask_setup(255,0xe4e4); packet_word(0x05000002);
        packet_word(0x72020000); packet_word(0x0201); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{13,20,200,43} ||
            packets.read(0x10003d00).value!=13 || packets.read(0x10003d10).value!=20 ||
            packets.read(0x10003d20).value!=30 || packets.read(0x10003d30).value!=40)
            throw std::runtime_error("Mask replacements mutated row state");
        const auto before_failed_launch=data_owner.state;
        upload_packet.clear(); packet_word(0x60010000); packet_word(7); packet_word(0x140007ff);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            data_owner.state!=before_failed_launch || packets.read(0x10003d00).value!=13)
            throw std::runtime_error("Failed native launch published mutable row or data state");
        // Restore normal configuration for subsequent unrelated launch fixtures.
        upload_packet.clear(); packet_word(0x0100ffff); packet_word(0x05000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget) throw std::runtime_error("Normal upload configuration failed");
        data_owner.state.data[1]={9,8,7,6};
        mask_setup(255,0xffaa5500); packet_word(0x01000401); packet_word(0x05000002);
        packet_word(0x720603fe); packet_word(0x0201); packet_word(0x0700002a); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1022]!=std::array<std::uint32_t,4>{11,21,31,41} ||
            data_owner.state.data[1023]!=std::array<std::uint32_t,4>{11,21,31,41} ||
            data_owner.state.vectors[0]!=std::array<std::uint32_t,4>{300,300,300,300} ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{9,8,7,6} ||
            data_owner.state.data[2]!=std::array<std::uint32_t,4>{13,23,33,43} ||
            data_owner.state.data[3]!=std::array<std::uint32_t,4>{13,23,33,43} ||
            packets.read(0x10003d00).value!=13 || packets.read(0x10003c30).value!=42)
            throw std::runtime_error("Masked filling lost source extent, updated rows, wrap or protected lanes");
        const auto before_fill_failure=data_owner.state;
        upload_packet.clear(); packet_word(0x20000000); packet_word(0); packet_word(0x72020000); packet_word(7);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            data_owner.state!=before_fill_failure || packets.read(0x10003c70).value!=0xffaa5500)
            throw std::runtime_error("Input-selected filling used invented data or published configuration");
        upload_packet.clear(); packet_word(0x70060000); packet_word(7);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state || data_owner.state!=before_fill_failure)
            throw std::runtime_error("Truncated fill source changed vector state");
        mask_setup(255,0x55555500); packet_word(0x01000401); packet_word(0x05000000);
        packet_word(0x72004000);
        for (unsigned i=0;i<16;++i) packet_word((i*4)|((i*4+1)<<8)|((i*4+2)<<16)|((i*4+3)<<24));
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[252]!=std::array<std::uint32_t,4>{63,63,63,63} ||
            data_owner.state.data[255]!=std::array<std::uint32_t,4>{10,20,30,40})
            throw std::runtime_error("Encoded-zero filling did not consume the bounded source count");
        upload_packet.clear(); packet_word(0x0100ffff); packet_word(0x05000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget) throw std::runtime_error("Upload configuration restoration failed");
        mask_setup(255,0x40404040);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget) throw std::runtime_error("V3 mask setup failed");
        unpack_and_launch(0x78010000,{1,2,3},{1,2,3,40});
        unpack_and_launch(0x79010000,{0xffff8000,0xdead7fff},{0xffff8000,0xffffffff,0x7fff,40});
        unpack_and_launch(0x79014000,{0xffff8000,0xdead7fff},{0x8000,0xffff,0x7fff,40});
        unpack_and_launch(0x7a010000,{0xde7fff80},{0xffffff80,0xffffffff,127,40});
        unpack_and_launch(0x7a014000,{0xde7fff80},{128,255,127,40});
        unpack_and_launch(0x78010000,{1,2,3},{11,22,33,40},2);
        if (packets.read(0x10003d30).value!=40) throw std::runtime_error("Masked V3 W changed row state");
        upload_packet.clear(); packet_word(0x05000000); packet_word(0x68010000);
        packet_word(1); packet_word(2); packet_word(3);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget || data_owner.state.data[0]!=std::array<std::uint32_t,4>{1,2,3,0})
            throw std::runtime_error("Unmasked V3 alignment-selected zero lane differs");
        unpack_and_launch(0x69010000,{0xffff8000,0xdead7fff},{0xffff8000,0xffffffff,0x7fff,0xffffdead});
        unpack_and_launch(0x69014000,{0xffff8000,0xdead7fff},{0x8000,0xffff,0x7fff,0xdead});
        // An eight-byte transferred tag precedes a fresh aligned payload.
        // W peeks at the following command without consuming it.
        upload_packet.clear(); packet_word(0); packet_word(0); packet_word(0x68020000);
        for (unsigned value:{1,2,3,4,5,6}) packet_word(value);
        packet_word(0x14000000);
        const std::array<nt::ee::NativePacketOrigin,2> origins{{{0,8},{8,0}}};
        if (packets.consume(1,upload_packet,false,origins)!=nt::ee::Stop::budget ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{4,5,6,0x14000000})
            throw std::runtime_error("V3 descriptor alignment or command lookahead changed");
        const auto before_missing_lookahead=data_owner.state;
        upload_packet.resize(upload_packet.size()-4);
        if (packets.consume(1,upload_packet,false,origins)!=nt::ee::Stop::unsupported_vector || data_owner.state!=before_missing_lookahead)
            throw std::runtime_error("Missing V3 lookahead was read or partially committed");
        upload_packet.clear(); packet_word(0x05000000); packet_word(0x20000000); packet_word(0x40);
        packet_word(0x7a020000); packet_word(0x04030201); packet_word(0xdead0605);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1]!=std::array<std::uint32_t,4>{4,5,6,0} || packets.read(0x10003c70).value!=0x40)
            throw std::runtime_error("V3 input-selected and masked lanes differ");
        const auto before_v3_failure=data_owner.state;
        upload_packet.clear(); packet_word(0x79010000); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::invalid_state || data_owner.state!=before_v3_failure)
            throw std::runtime_error("Truncated V3 consumed beyond its payload");
        upload_packet.clear(); packet_word(0x05000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget) throw std::runtime_error("V3 mode restoration failed");
        nt::vu::State integer_branch;
        nt::vu::Binding shared_binding(nt::vu::Program(0,{nt_vu_shared_blocks(),nt_vu_shared_count()}),0,
            {nt_vu_shared_image(),nt_vu_shared_size()});
        if (!shared_binding.upload(0,std::array{nt::vu::UploadView{0,{nt_vu_shared_image(),nt_vu_shared_size()}}}))
            throw std::runtime_error("Shared-state fixture binding failed");
        nt::ee::NativeVectorOwner macro_owner;
        macro_owner.storage.values[0].fill(0x3f800000);
        macro_owner.storage.values[1].fill(0xbf800000);
        macro_owner.storage.accumulator={11,22,33,44};
        macro_owner.storage.quotient=0x40400000; macro_owner.storage.clip=0x123456;
        macro_owner.storage.random=0x3f800321; macro_owner.storage.immediate=0x40000000;
        macro_owner.write(1,false,65535);
        nt::ee::NativeVuLaunch shared_launch(shared_binding,10,&macro_owner.storage);
        nt::ee::NativeVuSubmissions submissions;
        submissions.add(shared_launch); submissions.add(data_owner); submissions.connect(packets);
        upload_packet.clear(); packet_word(0x040000ab); packet_word(0x14000000);
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::budget || macro_owner.read(1,false).value!=0 ||
            macro_owner.storage.values[2]!=std::array<std::uint32_t,4>{} || macro_owner.read(17,false).value!=15 ||
            !(macro_owner.read(16,false).value&1) || shared_launch.state.input_itop!=0xab ||
            shared_launch.state.accumulator!=std::array<std::uint32_t,4>{11,22,33,44} ||
            shared_launch.state.quotient!=macro_owner.storage.quotient || shared_launch.state.clip!=macro_owner.storage.clip ||
            shared_launch.state.random!=0x3f800321 || shared_launch.state.immediate!=0x40000000 ||
            macro_owner.storage.accumulator!=shared_launch.state.accumulator || shared_launch.state.input_valid!=2 || (packets.read(0x10003800).value&0x80))
            throw std::runtime_error("Compiled VU0 failed shared vector, integer or flag handoff");
        macro_owner.storage.values[1].fill(0x40000000);
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::budget || macro_owner.read(1,false).value!=1 ||
            macro_owner.storage.values[2]!=std::array<std::uint32_t,4>{0x40400000,0x40400000,0x40400000,0x40400000})
            throw std::runtime_error("Compiled VU0 ignored newer macro state");
        const auto saved_vectors=macro_owner.storage.values;
        const auto saved_state=shared_launch.state;
        macro_owner.storage.active=true;
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::vector_pending ||
            shared_launch.state!=saved_state || macro_owner.storage.values!=saved_vectors)
            throw std::runtime_error("Active vector ownership was bypassed");
        macro_owner.storage.active=false;
        upload_packet.clear(); packet_word(0x14000001);
        if (packets.consume(0,upload_packet,false)!=nt::ee::Stop::unsupported_vector ||
            shared_launch.state!=saved_state || macro_owner.storage.values!=saved_vectors || macro_owner.read(1,false).value!=1)
            throw std::runtime_error("Rejected VU0 launch published partial state");
        integer_branch.integers[0]=5; integer_branch.integers[1]=7; integer_branch.data[0][2]=0xabcd0007;
        nt::ee::Memory macro_memory;
        nt::runtime::State macro_state; macro_state.pc=0x3f0000;
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::unsupported_vector || macro_state.executed)
            throw std::runtime_error("Unbound compiled VCALL retired");
        shared_launch.connect_macro(macro_memory);
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::syscall ||
            macro_state.pc!=0x3f0004 || macro_state.executed!=1 || macro_owner.read(1,false).value!=2)
            throw std::runtime_error("Compiled EE VCALL did not execute native VU code");
        macro_owner.write(27,false,0x201); // Address masks to missing entry 1.
        macro_state={}; macro_state.pc=0x3f0010;
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::unsupported_vector ||
            macro_state.executed || macro_owner.read(1,false).value!=2)
            throw std::runtime_error("Failed register launch retired or changed shared state");
        macro_owner.write(27,false,0x200);
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::syscall ||
            macro_state.executed!=1 || macro_owner.read(1,false).value!=3)
            throw std::runtime_error("Compiled VCALLMSR address selection failed");
        macro_owner.storage.active=true;
        macro_state={}; macro_state.pc=0x3f0020;
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::vector_pending ||
            macro_state.executed!=1 || macro_state.pc!=0x3f0024 || !macro_state.delay_slot)
            throw std::runtime_error("Pending VCALL lost EE delay-slot state");
        macro_owner.storage.active=false;
        if (nt_ee_generated_run(&macro_state,&macro_memory,8)!=nt::ee::Stop::syscall ||
            macro_state.executed!=2 || macro_state.pc!=0x3f002c || macro_state.delay_slot || macro_owner.read(1,false).value!=4)
            throw std::runtime_error("Retried compiled launch did not retire exactly once");
        shared_launch.connect_data(macro_memory); data_owner.connect_data(macro_memory);
        for (const auto address:{0x11004000U,0x11005000U,0xb1004000U,0x1100c000U}) {
            macro_state={}; macro_state.pc=0x400000; macro_state.gpr[4].lo=address;
            macro_state.gpr[8].lo=0x2222222211111111ULL; macro_state.gpr[8].hi=0x4444444433333333ULL;
            macro_state.gpr[10].lo=0x80000001; macro_state.gpr[12].lo=0xab;
            if (nt_ee_generated_run(&macro_state,&macro_memory,20)!=nt::ee::Stop::syscall ||
                macro_state.gpr[9].lo!=macro_state.gpr[8].lo || macro_state.gpr[9].hi!=macro_state.gpr[8].hi ||
                macro_state.gpr[11].lo!=0xffffffff80000001ULL || macro_state.gpr[13].lo!=0xab)
                throw std::runtime_error("Compiled EE VU data access failed");
            const auto& vectors=address==0x1100c000U ? data_owner.state.data : shared_launch.state.data;
            if (vectors[0]!=std::array<std::uint32_t,4>{0x11111111,0x22222222,0x33333333,0x44444444} ||
                vectors[1][0]!=0x80000001 || (vectors[1][1]&255)!=0xab)
                throw std::runtime_error("EE data accesses did not reach native VU storage");
        }
        const nt::ee::NativeLaunchView load_request{0,0,0};
        if (data_owner.submit(1,{},&load_request)!=nt::ee::Stop::budget || data_owner.state.vectors[0]!=data_owner.state.data[0])
            throw std::runtime_error("Compiled VU could not consume EE-written vectors");
        macro_state={}; macro_state.pc=0x400000; macro_state.gpr[4].lo=0x11004000;
        const auto before_busy_data=shared_launch.state;
        macro_owner.storage.active=true;
        if (nt_ee_generated_run(&macro_state,&macro_memory,20)!=nt::ee::Stop::vector_pending ||
            macro_state.executed || shared_launch.state!=before_busy_data)
            throw std::runtime_error("EE bypassed active VU data ownership");
        macro_owner.storage.active=false;
        macro_memory.vector_data[0].bytes=16;
        macro_state.pc=0x40000c;
        if (nt_ee_generated_run(&macro_state,&macro_memory,1)!=nt::ee::Stop::memory_fault || macro_state.executed)
            throw std::runtime_error("EE VU data access ignored supplied extent");
        macro_state.pc=0x400000; macro_state.gpr[4].lo=0x11000000;
        if (nt_ee_generated_run(&macro_state,&macro_memory,1)!=nt::ee::Stop::memory_fault || macro_state.executed)
            throw std::runtime_error("VU executable storage was exposed as writable native data");
        if (nt_vu_integer_branch(&integer_branch)!=nt::vu::Result::transfer || integer_branch.next_pair!=5 || integer_branch.integers[0]!=7)
            throw std::runtime_error("Native VU integer load branch visibility failed");
        nt::vu::State geometry;
        geometry.input_top=0xabcd; geometry.input_itop=0x1234; geometry.input_valid=3;
        geometry.vectors[0]={0x3f800000,0x40000000,0x40400000,99}; // 1,2,3
        geometry.vectors[1]={0x40800000,0x40a00000,0x40c00000,88}; // 4,5,6
        geometry.vectors[2][3]=77; geometry.accumulator[3]=66; geometry.mac=0x1111;
        const auto geometry_initial=geometry;
        if (nt_vu_geometry(&geometry)!=nt::vu::Result::complete || geometry.integers[0]!=0xabcd || geometry.integers[1]!=0x1234 ||
            geometry.accumulator!=std::array<std::uint32_t,4>{0x41400000,0x41400000,0x40a00000,66} ||
            geometry.vectors[2]!=std::array<std::uint32_t,4>{0xc0400000,0x40c00000,0xc0400000,77} || (geometry.mac&0x1111)!=0x1111)
            throw std::runtime_error("Native launch inputs or outer-product pair failed");
        for (unsigned valid=0;valid<3;++valid) {
            auto absent=geometry_initial; absent.input_valid=valid;
            const auto before=absent;
            if (nt_vu_geometry(&absent)!=nt::vu::Result::unsupported_context || absent!=before)
                throw std::runtime_error("Absent native launch input silently supplied");
        }
        auto extended_geometry=geometry_initial; extended_geometry.vectors[0][1]=0x7f800000;
        extended_geometry.vectors[1]={0,0x40a00000,0x3f800000,88};
        if (nt_vu_geometry(&extended_geometry)!=nt::vu::Result::complete || extended_geometry.accumulator[0]!=0x7f800000)
            throw std::runtime_error("Outer-product extended input failed");
        nt::vu::State p;
        p.vectors[0]={0x40400000,0x40800000,0,0x7f800000}; // Length 5; w is unused.
        p.vectors[1]={11,22,33,44}; p.scalar_p=0x3f800000; p.status=0xabc; p.mac=0x1234;
        const auto p_initial=p;
        if (nt_vu_p_0(&p)!=nt::vu::Result::transfer || p.next_pair!=3 || p.scalar_p!=0x3f800000 ||
            p.p_pending!=1 || p.pending_p!=0x3e4ccccd)
            throw std::runtime_error("Native reciprocal length did not preserve pending result");
        auto early_p=p;
        if (nt_vu_p_6(&early_p)!=nt::vu::Result::complete ||
            early_p.vectors[1]!=std::array<std::uint32_t,4>{0x3f800000,0x3f800000,0x3f800000,0x3f800000} ||
            early_p.scalar_p!=0x3e4ccccd || early_p.p_pending || early_p.p_read_delay)
            throw std::runtime_error("Early P read did not retain the published value before E drains the result");
        const std::array p_starts{nt_vu_p_26,nt_vu_p_52,nt_vu_p_83};
        const std::array p_finishes{nt_vu_p_29,nt_vu_p_55,nt_vu_p_86};
        for (unsigned n=0;n<3;++n) {
            auto timed=p_initial; timed.vectors[0][3]=0x41800000;
            if (p_starts[n](&timed)!=nt::vu::Result::transfer || !timed.p_pending)
                throw std::runtime_error("EFU producer did not carry publication across a native block");
            const auto expected=timed.pending_p;
            if (p_finishes[n](&timed)!=nt::vu::Result::complete || timed.vectors[2][0]!=p_initial.scalar_p ||
                timed.vectors[3][0]!=expected || timed.p_pending || timed.p_read_delay)
                throw std::runtime_error("EFU publication boundary selected the wrong P value");
        }
        if (nt_vu_p_3(&p)!=nt::vu::Result::complete || p.p_pending || p.scalar_p!=0x3e4ccccd ||
            p.vectors[1]!=std::array<std::uint32_t,4>{0x3e4ccccd,22,0x3e4ccccd,44} || p.status!=0xabc || p.mac!=0x1234)
            throw std::runtime_error("Native WAITP/MFP result or flags failed");
        auto zero_p=p_initial; zero_p.vectors[0]={};
        if (nt_vu_p_0(&zero_p)!=nt::vu::Result::transfer || nt_vu_p_3(&zero_p)!=nt::vu::Result::complete || zero_p.scalar_p!=0)
            throw std::runtime_error("Zero-length native EFU result failed");
        for (const auto tiny:{0x00800000U,0x80800000U,0x10000000U,0x90000000U}) {
            auto small=p_initial; small.vectors[0]={tiny,tiny,tiny,0};
            if (nt_vu_p_0(&small)!=nt::vu::Result::transfer || nt_vu_p_3(&small)!=nt::vu::Result::complete ||
                small.scalar_p || small.status!=p_initial.status || small.mac!=p_initial.mac)
                throw std::runtime_error("EFU squared underflow did not preserve zero-length and flag semantics");
        }
        for (const auto invalid:{0x7f800000U,0x7f7fffffU}) {
            auto bad_p=p_initial; bad_p.vectors[0][0]=invalid;
            const auto before=bad_p;
            if (nt_vu_p_0(&bad_p)!=nt::vu::Result::unsupported_numeric || bad_p!=before)
                throw std::runtime_error("Unsupported native EFU input did not roll back");
        }
        for (const auto input:{0U,0x00800000U,0x80800000U,0x3f800000U,0xbf800000U}) {
            nt::vu::State sine;
            sine.vectors[0]={0x7f800000,0x7f800000,input,0x7f800000};
            if (nt_vu_p_8(&sine)!=nt::vu::Result::complete || sine.p_pending ||
                std::abs(std::bit_cast<float>(sine.vectors[1][0])-std::sin(std::bit_cast<float>(input)))>0.000002F)
                throw std::runtime_error("Compiled ESIN/WAITP/MFP pipeline failed");
        }
        {
            nt::vu::State root; root.vectors[0][3]=0xc1800000; root.status=0x345;
            if (nt_vu_p_22(&root)!=nt::vu::Result::complete || root.vectors[1][0]!=0x40800000 || root.status!=0x345)
                throw std::runtime_error("EFU square root changed flags or failed magnitude semantics");
        }
        for (unsigned first=0;first<2;++first) for (unsigned second=0;second<2;++second) {
            nt::vu::State chained;
            chained.integers[0]=static_cast<std::uint16_t>(first);
            chained.integers[1]=static_cast<std::uint16_t>(second);
            const auto expected=second ? 20U : first ? 15U : 18U;
            if (nt_vu_p_12(&chained)!=nt::vu::Result::transfer || chained.next_pair!=expected ||
                chained.integers[2]!=(first ? 1U : 2U))
                throw std::runtime_error("Compiled chained branch executed the wrong delay or continuation");
        }
        nt::vu::State flags;
        {
            nt::vu::Program program(0,{nt_vu_mac_pipeline_blocks(),nt_vu_mac_pipeline_count()});
            nt::vu::State rolling; rolling.integers[0]=0xffff;
            // Invalid queue slots must stay invisible across idle/active transitions.
            rolling.pending_mac.fill(0xffff);
            rolling.pending_product_sticky.fill(15);
            rolling.vectors[9][0]=0x3f800000; // VF10.x = 1.
            const auto first=program.run(rolling,1);
            if (first.status!=nt::vu::RunStatus::budget_exhausted || rolling.next_pair!=4 || rolling.pending_mac_mask!=15)
                throw std::runtime_error("Outstanding MAC results did not survive compiled branch");
            const auto next=program.run(rolling,1);
            if (next.status!=nt::vu::RunStatus::complete || rolling.integers[1]!=8 || rolling.integers[2]!=128 ||
                rolling.integers[3]!=0 || rolling.integers[4]!=8 || rolling.pending_mac_mask || rolling.visible_mac!=8)
                throw std::runtime_error("Successive MAC flag reads did not observe their corresponding results");
        }
        flags.integers[0]=0xffff; flags.integers[2]=0x100;
        if (nt_vu_flags_0(&flags)!=nt::vu::Result::transfer || flags.mac_read_delay!=2 || flags.next_pair!=3)
            throw std::runtime_error("MAC visibility guard did not survive a native branch");
        const auto pending_flags=flags;
        if (nt_vu_flags_8(&flags)!=nt::vu::Result::complete || flags.integers[1]!=0 || flags.visible_mac!=8 || flags.pending_mac_mask)
            throw std::runtime_error("Early MAC read did not preserve the previously visible flags");
        flags=pending_flags;
        if (nt_vu_flags_3(&flags)!=nt::vu::Result::complete || flags.integers[1]!=8 ||
            flags.integers[3]!=0x108 || flags.integers[4]!=1 || flags.mac_read_delay)
            throw std::runtime_error("Native MAC comparison and bitwise operations failed");
        flags=pending_flags; flags.mac=0x10008;
        if (nt_vu_flags_3(&flags)!=nt::vu::Result::complete || flags.integers[4]!=1)
            throw std::runtime_error("MAC operations did not constrain flags to 16 bits");
        nt::vu::State status_flags; status_flags.status=0xf841;
        if (nt_vu_flags_10(&status_flags)!=nt::vu::Result::complete || status_flags.integers[5]!=0x841 ||
            status_flags.integers[6]!=0x843 || status_flags.integers[7]!=1)
            throw std::runtime_error("Compiled status operations lost mask, immediate or comparison semantics");
        status_flags.visible_status=0x402; status_flags.pending_mac_mask=8;
        status_flags.pending_mac[3]=8; status_flags.mac_read_delay=4;
        if (nt_vu_flags_10(&status_flags)!=nt::vu::Result::complete || status_flags.integers[5]!=0 ||
            status_flags.integers[6]!=0xc02 || status_flags.integers[7]!=0)
            throw std::runtime_error("Status reads used a newer arithmetic result before publication");
        const nt::vu::Program graphics_program(1,{nt_vu_graphics_blocks(),nt_vu_graphics_count()});
        nt::vu::State graphics_state;
        graphics_state.integers[0]=0xffff; // Address masks to final quadword; payload wraps.
        graphics_state.data[1023]={0x8001,0x10000000,0,0}; // Packed, one register, EOP.
        graphics_state.data[0]={11,22,33,44}; graphics_state.vectors[0]={55,66,77,88};
        if (graphics_program.run(graphics_state,10).status!=nt::vu::RunStatus::graphics_pending ||
            graphics_state.next_pair!=1 || graphics_state.graphics_address!=1023 || graphics_state.data[0][0]!=11)
            throw std::runtime_error("Native graphics boundary did not stop before continuation");
        const auto pending_graphics=graphics_state;
        nt::vu::Binding graphics_binding(nt::vu::Program(1,{nt_vu_graphics_batch_blocks(),nt_vu_graphics_batch_count()}),0,
            {nt_vu_graphics_batch_image(),nt_vu_graphics_batch_size()});
        if (!graphics_binding.upload(1,std::array{nt::vu::UploadView{0,{nt_vu_graphics_batch_image(),nt_vu_graphics_batch_size()}}}))
            throw std::runtime_error("Graphics batch fixture did not bind");
        nt::ee::NativeVuLaunch batch_owner(graphics_binding);
        batch_owner.state=graphics_state; batch_owner.state.graphics_pending=0;
        const auto before_batch=batch_owner.state;
        struct BatchSink { bool accept=false; unsigned calls=0; std::vector<nt::vu::GraphicsPacket> packets; } sink;
        const nt::vu::GraphicsBatchConsumer batch_consumer{&sink,[](void* raw,std::span<const nt::vu::GraphicsPacket> packets) {
            auto& sink=*static_cast<BatchSink*>(raw); ++sink.calls;
            if (!sink.accept) return false;
            sink.packets.assign(packets.begin(),packets.end()); return true;
        }};
        if (batch_owner.submit(1,{},&load_request)!=nt::ee::Stop::unsupported_vector || batch_owner.state!=before_batch)
            throw std::runtime_error("Graphics launch without consumer committed state");
        batch_owner.graphics=batch_consumer;
        if (batch_owner.submit(1,{},&load_request)!=nt::ee::Stop::unsupported_vector ||
            batch_owner.state!=before_batch || sink.calls!=1 || !sink.packets.empty())
            throw std::runtime_error("Rejected graphics batch committed execution");
        sink.accept=true;
        if (batch_owner.submit(1,{},&load_request)!=nt::ee::Stop::budget || sink.calls!=2 || sink.packets.size()!=2 ||
            sink.packets[0][1]!=std::array<std::uint32_t,4>{11,22,33,44} ||
            sink.packets[1][1]!=std::array<std::uint32_t,4>{55,66,77,88} || batch_owner.state.graphics_pending ||
            batch_owner.last_result.blocks!=2)
            throw std::runtime_error("Native graphics batch lost packet ordering or snapshots");
        nt::ee::NativeVuLaunch limited_graphics(graphics_binding,1);
        limited_graphics.state=before_batch; limited_graphics.graphics=batch_consumer;
        if (limited_graphics.submit(1,{},&load_request)!=nt::ee::Stop::unsupported_vector || sink.calls!=2 ||
            limited_graphics.state!=before_batch || limited_graphics.last_result.status!=nt::vu::RunStatus::budget_exhausted)
            throw std::runtime_error("Incomplete native launch delivered partial graphics");
        {
            std::array<nt::ee::NativeSubmissionStep,2> sequence;
            sequence[0].launch=load_request;
            sequence[1].launch=nt::ee::NativeLaunchView{2047,0,0};
            batch_owner.state=before_batch; sink.packets.clear();
            if (batch_owner.submit_sequence(1,sequence)!=nt::ee::Stop::unsupported_vector ||
                batch_owner.state!=before_batch || sink.calls!=2 || !sink.packets.empty())
                throw std::runtime_error("Later sequence failure published earlier state or packets");
            sequence[1].launch=load_request; sink.accept=false;
            if (batch_owner.submit_sequence(1,sequence)!=nt::ee::Stop::unsupported_vector ||
                batch_owner.state!=before_batch || sink.calls!=3 || !sink.packets.empty())
                throw std::runtime_error("Rejected sequence consumer committed private work");
            sink.accept=true;
            if (batch_owner.submit_sequence(1,sequence)!=nt::ee::Stop::budget || sink.calls!=4 || sink.packets.size()!=4 ||
                sink.packets[0][1]!=std::array<std::uint32_t,4>{11,22,33,44} ||
                sink.packets[1][1]!=std::array<std::uint32_t,4>{55,66,77,88} ||
                sink.packets[2][1]!=sink.packets[1][1] || sink.packets[3][1]!=sink.packets[1][1])
                throw std::runtime_error("Sequence ownership transfer lost immutable packet snapshots");
        }
        if (graphics_program.run(graphics_state,10).blocks!=0 ||
            nt::vu::submit_graphics(graphics_state,{})!=nt::vu::GraphicsResult::unavailable || graphics_state!=pending_graphics)
            throw std::runtime_error("Unconsumed graphics request advanced native execution");
        const nt::vu::GraphicsConsumer rejecting{nullptr,[](void*,std::span<const nt::graphics::Quadword>) { return false; }};
        if (nt::vu::submit_graphics(graphics_state,rejecting)!=nt::vu::GraphicsResult::unavailable || graphics_state!=pending_graphics)
            throw std::runtime_error("Rejected graphics packet lost continuation");
        std::vector<nt::graphics::Quadword> delivered;
        nt::vu::GraphicsConsumer consumer{&delivered,[](void* context,std::span<const nt::graphics::Quadword> packet) {
            static_cast<std::vector<nt::graphics::Quadword>*>(context)->assign(packet.begin(),packet.end()); return true;
        }};
        if (nt::vu::submit_graphics(graphics_state,consumer)!=nt::vu::GraphicsResult::accepted ||
            delivered.size()!=2 || delivered[1]!=std::array<std::uint32_t,4>{11,22,33,44} ||
            graphics_program.run(graphics_state,10).status!=nt::vu::RunStatus::complete || graphics_state.data[0][0]!=55)
            throw std::runtime_error("Native graphics packet submission/resume failed");
        auto malformed=pending_graphics; malformed.data[1023][0]=0xffff;
        const auto malformed_before=malformed;
        if (nt::vu::submit_graphics(malformed,consumer)!=nt::vu::GraphicsResult::invalid_packet || malformed!=malformed_before)
            throw std::runtime_error("Malformed graphics packet committed request state");
        std::array<nt::graphics::Quadword,8> packet_ring{};
        packet_ring[0]={1,0x34000000,0,0}; // REGLIST: 3 entries, 2 payload quadwords.
        packet_ring[3]={0x8001,0x08000000,0,0}; // IMAGE: 1 payload and EOP.
        if (nt::graphics::collect_packet(packet_ring,0).size()!=5)
            throw std::runtime_error("Native multi-tag graphics packet framing failed");
        bool missing_end=false;
        try { packet_ring={}; (void)nt::graphics::collect_packet(packet_ring,0); }
        catch (const std::invalid_argument&) { missing_end=true; }
        if (!missing_end) throw std::runtime_error("Unterminated graphics packet accepted");
        const nt::vu::Program end_graphics_program(1,{nt_vu_graphics_end_blocks(),nt_vu_graphics_end_count()});
        nt::vu::State end_graphics;
        end_graphics.integers[0]=4; end_graphics.data[4]={0x8000,0,0,0};
        const auto end_result=end_graphics_program.run(end_graphics,10);
        if (end_result.status!=nt::vu::RunStatus::graphics_pending || end_result.blocks!=1 ||
            end_graphics.integers[0]!=5 || end_graphics.graphics_address!=4 || !end_graphics.graphics_completes)
            throw std::runtime_error("XGKICK E delay-pair ordering failed");
        const auto end_pending=end_graphics;
        if (end_graphics_program.run(end_graphics,10).status!=nt::vu::RunStatus::graphics_pending ||
            nt::vu::submit_graphics(end_graphics,rejecting)!=nt::vu::GraphicsResult::unavailable || end_graphics!=end_pending)
            throw std::runtime_error("XGKICK E completed before acceptance");
        if (nt::vu::submit_graphics(end_graphics,consumer)!=nt::vu::GraphicsResult::accepted || delivered.size()!=1)
            throw std::runtime_error("XGKICK E packet was not accepted");
        for (unsigned attempt=0;attempt<2;++attempt) {
            const auto completed=end_graphics_program.run(end_graphics,10);
            if (completed.status!=nt::vu::RunStatus::complete || completed.blocks!=0 || end_graphics.integers[0]!=5)
                throw std::runtime_error("XGKICK E delay pair replayed after completion");
        }
        nt::vu::State clip;
        clip.clip=0xff0000; clip.vectors[0]={0x40000000,0xc0000000,0x3f800000,0};
        clip.vectors[1][3]=0xbf800000; clip.mac=123; clip.status=456;
        if (nt_vu_clip_0(&clip)!=nt::vu::Result::transfer || clip.clip!=0x249 || clip.clip_read_delay!=2)
            throw std::runtime_error("Native clip history or branch visibility failed");
        const auto clip_pending=clip;
        if (nt_vu_clip_18(&clip)!=nt::vu::Result::complete || clip.integers[1]!=9 || clip.visible_clip!=0x249)
            throw std::runtime_error("Early clip read did not preserve the older visible history");
        clip=clip_pending;
        if (nt_vu_clip_4(&clip)!=nt::vu::Result::complete || clip.integers[1]!=0x249 ||
            clip.integers[2]!=1 || clip.integers[3]!=1 || clip.integers[4]!=1 || clip.integers[5]!=0xdef ||
            clip.clip!=0xabcdef || clip.clip_read_delay || clip.mac!=123 || clip.status!=456)
            throw std::runtime_error("Native clip flag operations failed");
        if (nt::numeric::clip_flags(0x007fffff,0x00800000,0x80800000,0)!=0x24 ||
            nt::numeric::clip_flags(0,0x80000000,0,0x80000000)!=0 ||
            nt::numeric::clip_flags(0x3f800000,0xbf800000,0x3f800000,0x3f800000)!=0)
            throw std::runtime_error("Clip threshold, signed zero or equality semantics failed");
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
// Host-float lane helpers must reproduce the integer encoding and flags exactly
// whenever they do not request the exact path, and must request it at every
// range boundary the integer implementation handles specially.
void test_fast_lane_arithmetic() {
    std::uint64_t state=0x9e3779b97f4a7c15ULL;
    const auto next=[&] { state^=state<<13; state^=state>>7; state^=state<<17; return std::uint32_t(state); };
    // Bias exponents toward zero, the fast-range limits and exponent 255.
    constexpr std::array<std::uint32_t,14> edges{0,1,24,25,26,63,64,127,128,190,252,253,254,255};
    const auto operand=[&] {
        const auto r=next();
        const auto exponent=(r&3)==0 ? edges[(r>>2)%edges.size()] : (r>>2)&255;
        const auto mantissa=(r>>16)&1 ? next()&0x7fffff : ((r>>17)&1 ? 0x7fffff : 0);
        return (next()&0x80000000U)|(exponent<<23)|mantissa;
    };
    std::uint64_t compared=0,fast=0;
    const auto check=[&](std::uint64_t candidate,std::uint64_t exact,const char* operation,std::uint32_t a,std::uint32_t b,std::uint32_t c) {
        ++compared;
        if (candidate>>62) return;
        ++fast;
        if (candidate!=exact)
            throw std::runtime_error(std::string("Fast VU ")+operation+" differs: "+std::to_string(a)+" "+std::to_string(b)+" "+
                std::to_string(c)+" fast="+std::to_string(candidate)+" exact="+std::to_string(exact));
    };
    for (unsigned i=0;i<8000000;++i) {
        auto a=operand(),b=operand(),c=operand();
        // Near-cancellation and wide alignment cases for addition.
        if ((i&7)==0) b=(a^0x80000000U)+(next()%5)-2;
        if ((i&7)==1) b=(a&0x80000000U)|((((a>>23)&255)-std::min((a>>23)&255,24U+next()%8))<<23)|(next()&0x7fffff);
        const auto subtract=next()&1;
        check(nt_vu_add_fast(a,b,subtract),nt_ee_add_finite(a,b,subtract),"add",a,b,subtract);
        check(nt_vu_multiply_fast(a,b),nt_ee_multiply_finite(a,b),"multiply",a,b,0);
        check(nt_vu_multiply_accumulate_fast(c,a,b,subtract),nt_ee_multiply_accumulate_finite(c,a,b,subtract),"multiply-accumulate",c,a,b);
    }
    // The fast path must remain the common case for ordinary operands.
    if (fast*2<compared) throw std::runtime_error("Fast VU lane arithmetic rarely applies");
    for (const auto boundary:{0x7f800000U,0x7f800001U,0xff800000U})
        if (!(nt_vu_add_fast(boundary,0x3f800000U,0)>>62) || !(nt_vu_multiply_fast(boundary,0x3f800000U)>>62))
            throw std::runtime_error("Exponent 255 must use the exact VU path");
    if (!(nt_vu_multiply_fast(0x00800000U,0x00800000U)>>62) || !(nt_vu_multiply_fast(0x7f000000U,0x7f000000U)>>62))
        throw std::runtime_error("Product underflow and overflow must use the exact VU path");
}
int main() {
    try { test_fast_lane_arithmetic(); test_async_submission(); test_streamed_launches(false); test_streamed_launches(true); }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return run_core_tests();
}
