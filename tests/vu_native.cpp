#include <nt/vu_native.hpp>
#include <nt/vu_dispatch.hpp>
#include <nt/vu_binding.hpp>
#include <nt/ee_packet_setup.hpp>
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
#include <nt/vu_artifact.hpp>
#include <nt/vu_graphics.hpp>
#include <nt/vu_clip.hpp>
#include <nt/vu_quotient.hpp>
#include <iostream>
#include <stdexcept>

extern "C" std::uint64_t nt_ee_multiply_accumulate_finite(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" nt::vu::Result nt_vu_generated(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_data_blocks();
extern "C" std::uint32_t nt_vu_data_count();
extern "C" const std::uint8_t* nt_vu_data_image();
extern "C" std::uint32_t nt_vu_data_size();
extern "C" nt::vu::Result nt_vu_branch(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_call(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_end_false(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_end_true(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_start(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_finish(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_q_early(nt::vu::State*);
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
extern "C" nt::vu::Result nt_vu_flags_0(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_3(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_flags_8(nt::vu::State*);
extern "C" const nt::vu::Block* nt_vu_graphics_blocks();
extern "C" std::uint32_t nt_vu_graphics_count();
extern "C" const nt::vu::Block* nt_vu_graphics_end_blocks();
extern "C" std::uint32_t nt_vu_graphics_end_count();
extern "C" nt::vu::Result nt_vu_clip_0(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_clip_4(nt::vu::State*);
extern "C" nt::vu::Result nt_vu_clip_18(nt::vu::State*);
int main() {
    try {
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
            !(nt_ee_multiply_accumulate_finite(0x3f800000,0x00800000,0x3f000000,0)&(1ULL<<33)))
            throw std::runtime_error("VU multiply/accumulate rounding boundary failed");
        for (const auto invalid:{0x7f800000U,0x7fc12345U,0x7f7fffffU}) {
            auto rejected=initial; rejected.vectors[13][0]=invalid;
            const auto before=rejected;
            if (nt_vu_generated(&rejected)!=nt::vu::Result::unsupported_numeric || !(rejected==before))
                throw std::runtime_error("VU numeric failure partially committed a native region");
        }
        auto underflow=initial;
        underflow.vectors[13][0]=0x00800000; underflow.vectors[14][0]=0x3f000000;
        const auto before_underflow=underflow;
        if (nt_vu_generated(&underflow)!=nt::vu::Result::unsupported_numeric || !(underflow==before_underflow))
            throw std::runtime_error("VU intermediate product underflow was not atomic");
        for (const bool taken:{false,true}) {
            nt::vu::State branch; branch.integers[0]=5; branch.integers[1]=taken ? 5 : 6;
            if (nt_vu_branch(&branch)!=nt::vu::Result::transfer || branch.next_pair!=(taken ? 5U : 3U) ||
                branch.integers[0]!=6 || branch.integers[2]!=1 || branch.delayed_integer!=3 || branch.delayed_value!=0)
                throw std::runtime_error("Compiled VU branch/delay behavior failed");
        }
        nt::vu::State call; call.integers[0]=7;
        if (nt_vu_call(&call)!=nt::vu::Result::transfer || call.next_pair!=7 || call.integers[0]!=102 || call.integers[2]!=102)
            throw std::runtime_error("Compiled VU indirect link alias failed");
        call={}; call.integers[0]=9; call.delayed_integer=1; call.delayed_value=511;
        if (nt_vu_call(&call)!=nt::vu::Result::transfer || call.next_pair!=511)
            throw std::runtime_error("Compiled VU incoming delayed operand failed");
        nt::vu::State bad_branch; bad_branch.vectors[3][0]=0x7f800000;
        const auto before_branch=bad_branch;
        if (nt_vu_branch(&bad_branch)!=nt::vu::Result::unsupported_numeric || !(bad_branch==before_branch))
            throw std::runtime_error("Compiled VU branch numeric rollback failed");
        const std::array blocks{
            nt::vu::Block{5,nt_vu_end_true}, nt::vu::Block{0,nt_vu_branch},
            nt::vu::Block{3,nt_vu_end_false}, nt::vu::Block{100,nt_vu_call}};
        const nt::vu::Program native(0,blocks);
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
        const auto numeric=native.run(bad_branch,10);
        if (numeric.status!=nt::vu::RunStatus::unsupported_numeric || numeric.blocks!=1 || bad_branch!=before_branch)
            throw std::runtime_error("Native VU dispatcher lost numeric failure");
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
        if (nt_vu_q_early(&q)!=nt::vu::Result::unsupported_schedule || q!=pending_q)
            throw std::runtime_error("Unscheduled Q read did not stop atomically");
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
        nt::vu::State memory;
        memory.integers[1]=2; memory.integers[2]=1;
        memory.integers[7]=0x8001;
        memory.data[0]={9,9,9,9}; memory.vectors[0]={1,2,3,4}; memory.vectors[3]={5,6,7,8};
        const auto memory_before=memory;
        if (nt_vu_memory(&memory)!=nt::vu::Result::complete || memory.integers[0]!=0 || memory.integers[2]!=1 ||
            memory.vectors[1]!=std::array<std::uint32_t,4>{1,9,3,9} ||
            memory.vectors[2]!=std::array<std::uint32_t,4>{1,9,3,0x3f800000} ||
            memory.vectors[4]!=std::array<std::uint32_t,4>{5,6,7,8} ||
            memory.data[0]!=std::array<std::uint32_t,4>{5,6,7,0x8001} || memory.data[2][0]!=1 ||
            memory.data[1]!=std::array<std::uint32_t,4>{0x8001,0,0x8001,0} || memory.integers[5]!=0x8001 || memory.integers[6]!=8)
            throw std::runtime_error("Native VU load/store addressing failed");
        auto mapped_memory=memory_before; mapped_memory.integers[1]=1024;
        const auto mapped_before=mapped_memory;
        if (nt_vu_memory(&mapped_memory)!=nt::vu::Result::unsupported_memory || mapped_memory!=mapped_before)
            throw std::runtime_error("Native VU mapped memory failure was not atomic");
        auto unit1=memory_before; unit1.integers[1]=1024; unit1.data[1023]={11,12,13,14};
        if (nt_vu_memory1(&unit1)!=nt::vu::Result::complete ||
            unit1.data[256]!=std::array<std::uint32_t,4>{5,6,7,8} ||
            unit1.data[0]!=std::array<std::uint32_t,4>{1,9,3,0x8001} ||
            unit1.vectors[5]!=std::array<std::uint32_t,4>{11,12,13,14})
            throw std::runtime_error("Native VU1 memory extent or signed offset failed");
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
        auto trailing_packet=upload_packet; trailing_packet.insert(trailing_packet.end(),{0,0,0,0x7f});
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
        if (packets.consume(0,wrong_packet,false)!=nt::ee::Stop::unsupported_vector || binding.ready() || !binding1.ready())
            throw std::runtime_error("Rejected VU0 upload affected VU1 coverage");
        if (!binding.upload(0,std::array{nt::vu::UploadView{0,identity}}) || !binding.ready() || !binding1.ready())
            throw std::runtime_error("Both native VU units could not be bound together");
        nt::ee::NativeVuLaunch launch_owner(binding1);
        launch_owner.connect(packets);
        launch_owner.state.integers[0]=launch_owner.state.integers[1]=1;
        upload_packet.clear(); packet_word(0x03000020); packet_word(0x02000010); packet_word(0x04000009);
        packet_word(0x07000007); packet_word(0x14000000); packet_word(0);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            launch_owner.state.integers[4]!=1 || packets.read(0x10003c30).value!=7)
            throw std::runtime_error("Packet launch did not execute compiled VU1 code");
        if (launch_owner.state.input_top!=32 || launch_owner.state.input_itop!=9 ||
            launch_owner.state.input_valid!=3 || packets.read(0x10003cb0).value!=48 ||
            !(packets.read(0x10003c00).value&0x80))
            throw std::runtime_error("Native launch inputs or buffer selection failed");
        upload_packet.clear(); packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            launch_owner.state.input_top!=48 || packets.read(0x10003cb0).value!=32 ||
            (packets.read(0x10003c00).value&0x80))
            throw std::runtime_error("Second native launch did not alternate buffers");
        const auto completed_launch=launch_owner.state;
        packet_word(0x07000008);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || launch_owner.state!=completed_launch)
            throw std::runtime_error("Trailing command executed a partial VU launch");
        upload_packet.clear(); packet_word(0x07000008); packet_word(0x14000001);
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
        data_owner.connect(packets);
        upload_packet.clear(); packet_word(0x6c0203ff);
        for (unsigned value=1;value<=8;++value) packet_word(value);
        packet_word(0x14000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[1023]!=std::array<std::uint32_t,4>{1,2,3,4} ||
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
        upload_packet=data_packet; packet_word(0x7f000000);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_system || data_owner.state!=uploaded_state)
            throw std::runtime_error("Unsupported trailing command committed data");
        upload_packet.clear(); packet_word(0x03000040); packet_word(0x02000000); packet_word(0x6c018003);
        for (unsigned value=9;value<=12;++value) packet_word(value);
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::budget ||
            data_owner.state.data[67]!=std::array<std::uint32_t,4>{9,10,11,12})
            throw std::runtime_error("TOPS-relative upload destination failed");
        upload_packet.clear(); packet_word(0x01000201);
        upload_packet.insert(upload_packet.end(),data_packet.begin(),data_packet.end());
        if (packets.consume(1,upload_packet,false)!=nt::ee::Stop::unsupported_vector || packets.read(0x10003c40).value)
            throw std::runtime_error("Unsupported cycle mode committed configuration");
        nt::vu::State integer_branch;
        integer_branch.integers[0]=5; integer_branch.integers[1]=7; integer_branch.data[0][2]=0xabcd0007;
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
        auto bad_geometry=geometry_initial; bad_geometry.vectors[0][1]=0x7f800000;
        const auto bad_geometry_before=bad_geometry;
        if (nt_vu_geometry(&bad_geometry)!=nt::vu::Result::unsupported_numeric || bad_geometry!=bad_geometry_before)
            throw std::runtime_error("Outer-product numeric failure was not atomic");
        nt::vu::State p;
        p.vectors[0]={0x40400000,0x40800000,0,0x7f800000}; // Length 5; w is unused.
        p.vectors[1]={11,22,33,44}; p.scalar_p=0x3f800000; p.status=0xabc; p.mac=0x1234;
        const auto p_initial=p;
        if (nt_vu_p_0(&p)!=nt::vu::Result::transfer || p.next_pair!=3 || p.scalar_p!=0x3f800000 ||
            p.p_pending!=1 || p.pending_p!=0x3e4ccccd)
            throw std::runtime_error("Native reciprocal length did not preserve pending result");
        const auto pending_p=p;
        if (nt_vu_p_6(&p)!=nt::vu::Result::unsupported_schedule || p!=pending_p)
            throw std::runtime_error("Unscheduled P read was not atomic");
        if (nt_vu_p_3(&p)!=nt::vu::Result::complete || p.p_pending || p.scalar_p!=0x3e4ccccd ||
            p.vectors[1]!=std::array<std::uint32_t,4>{0x3e4ccccd,22,0x3e4ccccd,44} || p.status!=0xabc || p.mac!=0x1234)
            throw std::runtime_error("Native WAITP/MFP result or flags failed");
        auto zero_p=p_initial; zero_p.vectors[0]={};
        if (nt_vu_p_0(&zero_p)!=nt::vu::Result::transfer || nt_vu_p_3(&zero_p)!=nt::vu::Result::complete || zero_p.scalar_p!=0)
            throw std::runtime_error("Zero-length native EFU result failed");
        for (const auto invalid:{0x7f800000U,0x7f7fffffU,0x00800000U}) {
            auto bad_p=p_initial; bad_p.vectors[0][0]=invalid;
            const auto before=bad_p;
            if (nt_vu_p_0(&bad_p)!=nt::vu::Result::unsupported_numeric || bad_p!=before)
                throw std::runtime_error("Unsupported native EFU input did not roll back");
        }
        nt::vu::State flags;
        flags.integers[0]=0xffff; flags.integers[2]=0x100;
        if (nt_vu_flags_0(&flags)!=nt::vu::Result::transfer || flags.mac_read_delay!=2 || flags.next_pair!=3)
            throw std::runtime_error("MAC visibility guard did not survive a native branch");
        const auto pending_flags=flags;
        if (nt_vu_flags_8(&flags)!=nt::vu::Result::unsupported_schedule || flags!=pending_flags)
            throw std::runtime_error("Unproven MAC visibility was not rejected atomically");
        if (nt_vu_flags_3(&flags)!=nt::vu::Result::complete || flags.integers[1]!=8 ||
            flags.integers[3]!=0x108 || flags.integers[4]!=1 || flags.mac_read_delay)
            throw std::runtime_error("Native MAC comparison and bitwise operations failed");
        flags=pending_flags; flags.mac=0x10008;
        if (nt_vu_flags_3(&flags)!=nt::vu::Result::complete || flags.integers[4]!=1)
            throw std::runtime_error("MAC operations did not constrain flags to 16 bits");
        const nt::vu::Program graphics_program(1,{nt_vu_graphics_blocks(),nt_vu_graphics_count()});
        nt::vu::State graphics_state;
        graphics_state.integers[0]=0xffff; // Address masks to final quadword; payload wraps.
        graphics_state.data[1023]={0x8001,0x10000000,0,0}; // Packed, one register, EOP.
        graphics_state.data[0]={11,22,33,44}; graphics_state.vectors[0]={55,66,77,88};
        if (graphics_program.run(graphics_state,10).status!=nt::vu::RunStatus::graphics_pending ||
            graphics_state.next_pair!=1 || graphics_state.graphics_address!=1023 || graphics_state.data[0][0]!=11)
            throw std::runtime_error("Native graphics boundary did not stop before continuation");
        const auto pending_graphics=graphics_state;
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
        if (nt_vu_clip_18(&clip)!=nt::vu::Result::unsupported_schedule || clip!=clip_pending)
            throw std::runtime_error("Early clip read did not roll back");
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
