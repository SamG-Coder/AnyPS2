#include "render_loop_fixture.hpp"
#include <nt/vector_region_codegen.hpp>
#include <nt/element_packet.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
nt::Bytes program(unsigned variant) {
    nt::Bytes bytes;
    const auto pair=[&](std::uint32_t low=render_fixture::lower_nop,std::uint32_t up=render_fixture::upper_nop) {
        render_fixture::word(bytes,low); render_fixture::word(bytes,up);
    };
    const auto load=[](unsigned vf,unsigned vi){return 0x80000000U|(15U<<21)|(vf<<16)|(vi<<11)|(13U<<6)|0x3cU;};
    const auto store=[](unsigned vf){return 0x80000000U|(15U<<21)|(4U<<16)|(vf<<11)|(13U<<6)|0x3dU;};
    const bool pipelined=variant==1 || variant==2,arithmetic=variant==4,quotient=variant==5;
    if (pipelined) { pair(store(2)); pair(store(6)); }
    if (variant==2) pair(render_fixture::lower_nop,(15U<<21)|(7U<<16)|(6U<<11)|0x1fdU); // ABS VF7, VF6
    pair(load(1,3)); pair(load(2,5));
    if (arithmetic) {
        // ACC = VF1*VF20; VF7 = ACC + VF2*VF21; position = FTOI4(VF7). Flags and ACC are live out.
        pair(render_fixture::lower_nop,(15U<<21)|(20U<<16)|(1U<<11)|0x2beU); // MULA
        pair(render_fixture::lower_nop,(15U<<21)|(21U<<16)|(2U<<11)|(7U<<6)|0x29U); // MADD
        pair(render_fixture::lower_nop,(15U<<21)|(6U<<16)|(7U<<11)|0x17dU); // FTOI4
    } else if (quotient) {
        // Q = VF1.x/VF20.x; WAITQ publishes it before MULq reads it in the same
        // pair; position = FTOI4(VF1*Q). Q and flags are live out.
        pair(0x80000000U|(1U<<11)|(20U<<16)|(14U<<6)|60U); // DIV Q, VF1.x, VF20.x
        pair(0x80000000U|(14U<<6)|63U,(15U<<21)|(1U<<11)|(7U<<6)|28U); // WAITQ; MULq VF7, VF1, Q
        pair(render_fixture::lower_nop,(15U<<21)|(6U<<16)|(7U<<11)|0x17dU); // FTOI4
    } else pair(render_fixture::lower_nop,(15U<<21)|(6U<<16)|(1U<<11)|0x17dU); // FTOI4
    if (!pipelined) { pair(store(2)); pair(store(6)); }
    pair((9U<<25)|(9U<<16)|(9U<<11)|1U);
    // The arithmetic variant compares the counter through the integer backup.
    if (!arithmetic) { pair(); pair(); }
    const int displacement=-int(bytes.size()/8)-1;
    pair((41U<<25)|(9U<<11)|(unsigned(displacement)&0x7ffU)); pair();
    pair(render_fixture::lower_nop,0x400002ffU); pair(); // caller continuation
    return bytes;
}
void save(const std::string& name,const std::string& text) {
    nt::write_file(name,nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
void reject(std::vector<nt::vu::Pair> pairs,unsigned unit,std::vector<unsigned> entries,unsigned head,const char* message) {
    try { (void)nt::render::analyze_vector_region(pairs,unit,entries,head); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::invalid_argument("Expected output prefix");
        for (unsigned variant=0;variant<6;++variant) {
            const unsigned unit=variant==3 ? 0:1,head=variant==3 ? 32:0;
            auto pairs=nt::vu::frame(program(variant),unit,head);
            const std::array entries{head};
            const auto plan=nt::render::analyze_vector_region(pairs,unit,entries,head);
            // Flag variants add two exact replay iterations; the quotient settles in one.
            if (plan.startup!=unsigned(variant==1 || variant==2) || plan.drain!=(variant==2 ? 2U : variant>=4 ? 3U : 1U))
                throw std::runtime_error("Incorrect startup/drain proof");
            if ((variant==4)!=(plan.range.trip_offset==1) || (variant>=4)==plan.loop.flags.empty())
                throw std::runtime_error("Incorrect flag or integer-backup proof");
            std::ostringstream boundary,elements,binding,gpu,packet;
            const auto symbol="region_"+std::to_string(variant);
            nt::render::emit_vector_region_llvm(boundary,elements,binding,plan,symbol);
            nt::render::emit_element_kernel_llvm(gpu,plan.loop,unit,true);
            // Each iteration stores RGBAQ then XYZ2 through VI4: a two-register PACKED record.
            nt::render::emit_element_packet_llvm(packet,nt::render::bind_element_packet(plan.loop,4,0,0x2000000000000000ULL,0x51),symbol+"_packet");
            const auto prefix=std::string(argv[1])+"-"+std::to_string(variant);
            save(prefix+".boundary.ll",boundary.str()); save(prefix+".elements.ll",elements.str());
            save(prefix+".binding.ll",binding.str()); save(prefix+".gpu.ll",gpu.str());
            save(prefix+".packet.ll",packet.str());
            nt::write_file(prefix+".bin",program(variant));
            const auto same=[&](std::vector<nt::vu::Pair> image,std::vector<unsigned> exported,const char* message) {
                const auto other=nt::render::analyze_vector_region(image,unit,exported,head);
                if (other.startup!=plan.startup || other.drain!=plan.drain || other.exit!=plan.exit) throw std::runtime_error(message);
            };
            // Other paths into the body run original compiled code up to the head.
            same(pairs,{head,head+1},"Exported interior entry changed the region proof");
            auto incomplete=pairs; incomplete.resize(incomplete.size()-2);
            reject(incomplete,unit,{head},head,"Undeclared exit continuation accepted");
            auto prefixed=nt::Bytes{};
            render_fixture::word(prefixed,32U<<25); render_fixture::word(prefixed,render_fixture::upper_nop);
            const auto loop_bytes=program(variant); prefixed.insert(prefixed.end(),loop_bytes.begin(),loop_bytes.end());
            reject(nt::vu::frame(prefixed,unit,head),unit,{head},head+1,"External delay-slot entry accepted");
            auto invalid=program(variant);
            render_fixture::replace(invalid,unsigned(invalid.size()/8)-1,false,(36U<<25)|(1U<<11));
            same(nt::vu::frame(invalid,unit,head),{head},"Register jump changed the region proof");
            invalid=program(variant);
            // External tail branch to an interior pair of the selected loop.
            const auto source=unsigned(invalid.size()/8)-2;
            render_fixture::replace(invalid,source,true,render_fixture::upper_nop);
            render_fixture::replace(invalid,source,false,(32U<<25)|((1U-source-1U)&0x7ffU));
            same(nt::vu::frame(invalid,unit,head),{head},"External interior branch changed the region proof");
            invalid=program(variant);
            // OPMULA status reduces a MAC word that keeps earlier W flags: not reconstructed.
            render_fixture::replace(invalid,0,true,(14U<<21)|(1U<<16)|(2U<<11)|0x2feU);
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"Unreconstructed flags accepted");
            invalid=program(variant);
            render_fixture::replace(invalid,0,true,(15U<<21)|(8U<<16)|(8U<<11)|0x1fdU);
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"Unbounded live-out recurrence accepted");
        }
        {
            // Two output streams (VI4, VI3) and an in-place read one row ahead
            // of the VI4 stream: each row is read before the iteration writing it.
            const auto streams=[](int ahead) {
                nt::Bytes bytes;
                const auto pair=[&](std::uint32_t low=render_fixture::lower_nop,std::uint32_t up=render_fixture::upper_nop) {
                    render_fixture::word(bytes,low); render_fixture::word(bytes,up);
                };
                pair((15U<<21)|(1U<<16)|(4U<<11)|(unsigned(ahead)&0x7ffU)); // LQ VF1, ahead(VI4)
                pair(0x80000000U|(15U<<21)|(2U<<16)|(5U<<11)|(13U<<6)|0x3cU); // LQI VF2, (VI5++)
                pair(render_fixture::lower_nop,(15U<<21)|(6U<<16)|(1U<<11)|0x17dU); // FTOI4 VF6, VF1
                pair(0x80000000U|(15U<<21)|(4U<<16)|(2U<<11)|(13U<<6)|0x3dU); // SQI VF2, (VI4++)
                pair(0x80000000U|(15U<<21)|(3U<<16)|(6U<<11)|(13U<<6)|0x3dU); // SQI VF6, (VI3++)
                pair((9U<<25)|(9U<<16)|(9U<<11)|1U); pair(); pair();
                const int displacement=-int(bytes.size()/8)-1;
                pair((41U<<25)|(9U<<11)|(unsigned(displacement)&0x7ffU)); pair();
                pair(render_fixture::lower_nop,0x400002ffU); pair();
                return bytes;
            };
            const std::array entries{0U};
            const auto plan=nt::render::analyze_vector_region(nt::vu::frame(streams(1),1,0),1,entries,0);
            if (plan.range.outputs.size()!=2 || std::none_of(plan.range.reads.begin(),plan.range.reads.end(),
                    [](const auto& read){return read.ordered!=0;}))
                throw std::runtime_error("Multiple output streams or the in-place read ahead were not proved");
            // Reading the row the previous iteration wrote carries a value through memory.
            reject(nt::vu::frame(streams(-1),1,0),1,{0},0,"Read behind an output stream accepted");
        }
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
