#include "render_loop_fixture.hpp"
#include <nt/vector_region_codegen.hpp>
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
    const bool pipelined=variant==1 || variant==2;
    if (pipelined) { pair(store(2)); pair(store(6)); }
    if (variant==2) pair(render_fixture::lower_nop,(15U<<21)|(7U<<16)|(6U<<11)|0x1fdU); // ABS VF7, VF6
    pair(load(1,3)); pair(load(2,5));
    pair(render_fixture::lower_nop,(15U<<21)|(6U<<16)|(1U<<11)|0x17dU); // FTOI4
    if (!pipelined) { pair(store(2)); pair(store(6)); }
    pair((9U<<25)|(9U<<16)|(9U<<11)|1U);
    pair(); pair();
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
        for (unsigned variant=0;variant<4;++variant) {
            const unsigned unit=variant==3 ? 0:1,head=variant==3 ? 32:0;
            auto pairs=nt::vu::frame(program(variant),unit,head);
            const std::array entries{head};
            const auto plan=nt::render::analyze_vector_region(pairs,unit,entries,head);
            if (plan.startup!=unsigned(variant==1 || variant==2) || plan.drain!=(variant==2 ? 2U:1U))
                throw std::runtime_error("Incorrect startup/drain proof");
            std::ostringstream boundary,elements,binding,gpu;
            const auto symbol="region_"+std::to_string(variant);
            nt::render::emit_vector_region_llvm(boundary,elements,binding,plan,symbol);
            nt::render::emit_element_kernel_llvm(gpu,plan.loop,unit,true);
            const auto prefix=std::string(argv[1])+"-"+std::to_string(variant);
            save(prefix+".boundary.ll",boundary.str()); save(prefix+".elements.ll",elements.str());
            save(prefix+".binding.ll",binding.str()); save(prefix+".gpu.ll",gpu.str());
            nt::write_file(prefix+".bin",program(variant));
            reject(pairs,unit,{head,head+1},head,"Exported interior entry accepted");
            auto incomplete=pairs; incomplete.resize(incomplete.size()-2);
            reject(incomplete,unit,{head},head,"Undeclared exit continuation accepted");
            auto prefixed=nt::Bytes{};
            render_fixture::word(prefixed,32U<<25); render_fixture::word(prefixed,render_fixture::upper_nop);
            const auto loop_bytes=program(variant); prefixed.insert(prefixed.end(),loop_bytes.begin(),loop_bytes.end());
            reject(nt::vu::frame(prefixed,unit,head),unit,{head},head+1,"External delay-slot entry accepted");
            auto invalid=program(variant);
            render_fixture::replace(invalid,unsigned(invalid.size()/8)-1,false,(36U<<25)|(1U<<11));
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"Indirect entry uncertainty accepted");
            invalid=program(variant);
            // External tail branch to an interior pair of the selected loop.
            const auto source=unsigned(invalid.size()/8)-2;
            render_fixture::replace(invalid,source,true,render_fixture::upper_nop);
            render_fixture::replace(invalid,source,false,(32U<<25)|((1U-source-1U)&0x7ffU));
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"External interior branch accepted");
            invalid=program(variant);
            render_fixture::replace(invalid,0,true,(15U<<21)|(1U<<16)|(2U<<11)|(8U<<6)|0x28U);
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"Unreconstructed flags accepted");
            invalid=program(variant);
            render_fixture::replace(invalid,0,true,(15U<<21)|(8U<<16)|(8U<<11)|0x1fdU);
            reject(nt::vu::frame(invalid,unit,head),unit,{head},head,"Unbounded live-out recurrence accepted");
        }
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
