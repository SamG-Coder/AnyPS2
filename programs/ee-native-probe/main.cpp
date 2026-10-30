#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/binary.hpp>
#include <nt/ee_file_rpc.hpp>
#include <memory>
#include <nt/vulkan_renderer.hpp>
#include <charconv>
#include <iostream>
#include <stdexcept>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
namespace {
struct NativeGpu {
    std::unique_ptr<nt::graphics::VulkanRenderer> renderer;
    std::string error;
    static nt::ee::Stop synchronize(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        try {
            if (!self.renderer) self.renderer=std::make_unique<nt::graphics::VulkanRenderer>();
            self.renderer->synchronize();
            return nt::ee::Stop::budget;
        } catch (const std::exception& error) {
            self.error=error.what(); return nt::ee::Stop::unsupported_system;
        }
    }
};
bool trace_services=false;
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
    const auto stop=nt_ee_generated_run(state,memory,budget);
    if (trace_services && stop==nt::ee::Stop::syscall) {
        std::cerr<<"service pc="<<nt::hex32(state->pc)<<" number="<<state->gpr[3].lo;
        for (unsigned i=4;i<8;++i) std::cerr<<" a"<<(i-4)<<"="<<nt::hex32(static_cast<std::uint32_t>(state->gpr[i].lo));
        std::cerr<<'\n';
    }
    return stop;
}
}
int main(int argc,char** argv) {
    if (argc<2) {
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [--trace-services] [--disc <image.iso>]\n"; return 2;
    }
    try {
        std::uint64_t budget=1000000;
        std::filesystem::path disc;
        bool has_budget=false;
        for (int index=2;index<argc;++index) {
            const std::string_view text=argv[index];
            if (text=="--trace-services") { trace_services=true; continue; }
            if (text=="--disc") {
                if (!disc.empty() || index+1==argc) throw std::runtime_error("Expected one disc image path");
                disc=argv[++index]; continue;
            }
            if (has_budget) throw std::runtime_error("Unknown probe option");
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),budget);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid operation budget or option");
            has_budget=true;
        }
        auto ram=nt::read_file(argv[1],0x2000000);
        if (ram.size()!=ANYPS2_EE_DATA_SIZE || nt::fingerprint(ram)!=ANYPS2_EE_DATA_HASH)
            throw std::runtime_error("Converted program data mismatch");
        ram.resize(0x2000000,0);
        nt::ee::Memory memory{ram.data(),ram.size()};
        NativeGpu gpu;
        nt::ee::NativePlatform platform; platform.attach(memory);
        platform.graphics.context=&gpu; platform.graphics.synchronize=NativeGpu::synchronize;
        nt::ee::NativeSifCommandPeer command_peer(platform.sif_link,memory,nt_ee_generated_code_overlap,0x1000,&platform.sif_receive);
        std::unique_ptr<nt::ee::NativeFileRpc> files;
        if (!disc.empty()) {
            files=std::make_unique<nt::ee::NativeFileRpc>(disc);
            if (!command_peer.register_service(*files)) throw std::runtime_error("Cannot register native disc service");
        }
        if (!command_peer.connect(platform.sif_outbound)) throw std::runtime_error("Cannot connect native command peer");
        auto state=nt::ee::NativePlatform::initial_state(ANYPS2_EE_ENTRY);
        nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
        const auto stop=nt::ee::run(execute,state,memory,budget,services);
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo<<" syscall="<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write || stop==nt::ee::Stop::unpublished_code || (stop==nt::ee::Stop::unsupported_system && memory.fault_address))
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code) std::cout<<" return_address="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[31].lo));
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x74)
            std::cout<<" requested_syscall="<<state.gpr[4].lo
                     <<" uncompiled_handler="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x20)
            std::cout<<" uncompiled_thread_entry="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unsupported_system && command_peer.last_command())
            std::cout<<" native_peer_command="<<nt::hex32(command_peer.last_command())<<" option="<<command_peer.last_option();
        if (!gpu.error.empty()) std::cerr<<"graphics: "<<gpu.error<<'\n';
        if (trace_services) {
            if (gpu.renderer) std::cout<<" graphics_resets="<<platform.graphics.resets()<<" gpu=\""<<gpu.renderer->device_name()<<"\"";
            if (files) {
                const auto stats=files->statistics();
                std::cout<<" file_requests="<<stats.requests<<" file_opens="<<stats.opens<<" file_bytes="<<stats.bytes_read<<" file_method="<<stats.last_method;
            }
            if (const auto server=command_peer.last_unavailable_server())
                std::cout<<" unavailable_rpc_server="<<nt::hex32(*server);
            std::cout<<" status="<<nt::hex32(state.cop0_status)<<" pending="<<nt::hex32(memory.pending_interrupts)
                     <<" a0="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[4].lo))
                     <<" a1="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        }
        std::cout<<'\n';
        return stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
