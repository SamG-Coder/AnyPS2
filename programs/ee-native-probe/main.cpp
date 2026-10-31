#include "capture.hpp"
#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/binary.hpp>
#include <nt/ee_file_rpc.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <memory>
#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing.hpp>
#include <charconv>
#include <iostream>
#include <fstream>
#include <stdexcept>
#ifdef ANYPS2_NATIVE_IOP
#include "iop_reset.hpp"
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
#include <nt/vu_artifact.hpp>
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
#endif
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
namespace {
struct VuCapture {
    unsigned unit=0,destination=0;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> packet;
    static void observe_packet(void* context,unsigned,std::span<const std::uint8_t> data) {
        auto& self=*static_cast<VuCapture*>(context);
        if (self.packet.empty()) self.packet.assign(data.begin(),data.end());
    }
    static void observe(void* context,unsigned unit,unsigned destination,std::span<const std::uint8_t> data) {
        auto& self=*static_cast<VuCapture*>(context);
        if (!self.bytes.empty()) return;
        self.unit=unit; self.destination=destination; self.bytes.assign(data.begin(),data.end());
    }
    void save(const std::filesystem::path& directory) const {
        if (bytes.empty()) return;
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("VU capture directory already exists");
        nt::write_file(directory/"upload.bin",bytes);
        if (!packet.empty()) nt::write_file(directory/"packet.bin",packet);
        std::ofstream metadata(directory/"upload.txt");
        if (!(metadata<<"unit="<<unit<<"\ndestination="<<destination<<"\ninstructions="<<bytes.size()/8
                      <<"\nfingerprint="<<nt::fingerprint(bytes)<<"\n")) throw std::runtime_error("Cannot write VU capture metadata");
    }
};
struct NativeGpu {
    std::unique_ptr<nt::graphics::VulkanRenderer> renderer;
    nt::graphics::NativeDrawing drawing;
    std::string error;
    static bool submit(void* raw,std::span<const nt::graphics::NativeDrawing::Packet> packets) {
        auto& self=*static_cast<NativeGpu*>(raw);
        if (synchronize(raw)!=nt::ee::Stop::budget) return false;
        try { self.drawing.submit(*self.renderer,packets); return true; }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
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
#ifdef ANYPS2_NATIVE_IOP
probe::IopReset* active_iop=nullptr;
#endif
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
#ifdef ANYPS2_NATIVE_IOP
    if (active_iop) {
        const auto result=active_iop->pump();
        if (result!=nt::ee::Stop::budget) return result;
    }
#endif
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
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [--trace-services] [--disc <image.iso>] [--capture-vu <new-private-directory>] [--capture-stop <new-private-directory>]\n";
#ifdef ANYPS2_NATIVE_IOP
        std::cerr<<"Linked IOP investigation: --probe-iop-reset <module.data> (stops without acknowledging reset)\n";
        std::cerr<<"Explicit selected-bundle execution: --iop-reset-session <module.data> --disc <image.iso>\n";
#endif
        return 2;
    }
    try {
        std::uint64_t budget=1000000;
        std::filesystem::path disc,capture_directory,stop_directory;
        bool has_budget=false;
#ifdef ANYPS2_NATIVE_IOP
        std::filesystem::path iop_data;
        bool iop_session=false;
#endif
        for (int index=2;index<argc;++index) {
            const std::string_view text=argv[index];
            if (text=="--trace-services") { trace_services=true; continue; }
#ifdef ANYPS2_NATIVE_IOP
            if (text=="--probe-iop-reset" || text=="--iop-reset-session") {
                if (!iop_data.empty() || index+1==argc) throw std::runtime_error("Expected one linked IOP data path");
                iop_session=text=="--iop-reset-session";
                iop_data=argv[++index]; continue;
            }
#endif
            if (text=="--capture-stop") {
                if (!stop_directory.empty() || index+1==argc)
                    throw std::runtime_error("Expected one private stop capture directory");
                stop_directory=argv[++index];
                if (std::filesystem::exists(stop_directory))
                    throw std::runtime_error("Stop capture directory already exists");
                continue;
            }
            if (text=="--capture-vu") {
                if (!capture_directory.empty() || index+1==argc) throw std::runtime_error("Expected one private VU capture directory");
                capture_directory=argv[++index];
                if (std::filesystem::exists(capture_directory)) throw std::runtime_error("VU capture directory already exists");
                continue;
            }
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
        platform.packet_setup.graphics={&gpu,NativeGpu::submit,[](void* raw) {
            return NativeGpu::synchronize(raw)==nt::ee::Stop::budget;
        }};
        VuCapture capture;
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        nt::ee::NativeVuUploads compiled_uploads;
        nt::ee::NativeVuSubmissions compiled_submissions;
#endif
#ifdef ANYPS2_NATIVE_VU
        auto compiled_vu=nt::vu::linked_binding();
        compiled_uploads.add(compiled_vu);
        nt::ee::NativeVuLaunch compiled_vu_launch(compiled_vu,100000,&platform.vector_owner.storage);
        compiled_submissions.add(compiled_vu_launch);
        compiled_vu_launch.connect_macro(memory);
        compiled_vu_launch.connect_data(memory);
#endif
#ifdef ANYPS2_NATIVE_VU1
        auto compiled_vu1=nt::vu::linked_binding1();
        compiled_uploads.add(compiled_vu1);
        nt::ee::NativeVuLaunch compiled_vu1_launch(compiled_vu1);
        compiled_submissions.add(compiled_vu1_launch);
        compiled_vu1_launch.connect_data(memory);
        compiled_vu1_launch.graphics={&gpu,NativeGpu::submit};
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        compiled_uploads.connect(platform.packet_setup);
        compiled_submissions.connect(platform.packet_setup);
#endif
        if (!capture_directory.empty()) {
            platform.packet_setup.upload_context=&capture;
            platform.packet_setup.uncompiled_upload=VuCapture::observe;
            platform.packet_setup.uncompiled_packet=VuCapture::observe_packet;
        }
        platform.graphics.context=&gpu; platform.graphics.synchronize=NativeGpu::synchronize;
        platform.graphics.reset_drawing=[](void* context) {
            static_cast<NativeGpu*>(context)->drawing.reset(); return nt::ee::Stop::budget;
        };
        nt::ee::NativeSifCommandPeer command_peer(platform.sif_link,memory,nt_ee_generated_code_overlap,0x1000,&platform.sif_receive);
#ifdef ANYPS2_NATIVE_IOP
        std::unique_ptr<probe::IopReset> iop_reset;
        if (!iop_data.empty()) {
            iop_reset=std::make_unique<probe::IopReset>(iop_data,disc,memory,platform,nt_ee_generated_code_overlap,iop_session);
            if (!command_peer.reset_owner(iop_reset->owner())) throw std::runtime_error("Cannot bind compiled IOP reset probe");
            active_iop=iop_reset.get();
        }
#endif
        std::unique_ptr<nt::ee::NativeFileRpc> files;
        std::unique_ptr<nt::ee::NativeCdvdInitRpc> cdvd;
        if (!disc.empty()) {
            files=std::make_unique<nt::ee::NativeFileRpc>(disc);
            cdvd=std::make_unique<nt::ee::NativeCdvdInitRpc>(disc);
            if (!command_peer.register_service(*cdvd)) throw std::runtime_error("Cannot register native CD/DVD initialization");
            if (!command_peer.register_service(*files)) throw std::runtime_error("Cannot register native disc service");
        }
        if (!command_peer.connect(platform.sif_outbound)) throw std::runtime_error("Cannot connect native command peer");
        auto state=nt::ee::NativePlatform::initial_state(ANYPS2_EE_ENTRY);
        nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
#ifdef ANYPS2_NATIVE_IOP
        if (iop_reset) services.idle_pump=[&] { return iop_reset->pump(); };
#endif
        const auto stop=nt::ee::run(execute,state,memory,budget,services);
#ifdef ANYPS2_NATIVE_IOP
        if (iop_reset && iop_reset->invoked()) {
            const auto result=iop_reset->result();
            std::cout<<"iop_reset_accepted="<<iop_reset->accepted()<<" entries_returned="<<iop_reset->returned()
                     <<" startup_finished="<<iop_reset->finished()<<" executed="<<iop_reset->executed()
                     <<" reason="<<static_cast<unsigned>(result.reason)<<" pc="<<nt::hex32(result.pc)
                     <<" address="<<nt::hex32(result.address)<<" trap="<<nt::hex32(result.trap_code)
                     <<" last_bind="<<nt::hex32(iop_reset->last_server())
                     <<" session_cdvd_initialized="<<iop_reset->cdvd_initialized()
                     <<" cdvd_command="<<iop_reset->cdvd_command()
                     <<" cdvd_media="<<iop_reset->cdvd_media()
                     <<" cdvd_argument="<<iop_reset->cdvd_argument()
                     <<" cdvd_ready_calls="<<iop_reset->cdvd_ready_calls()
                     <<" cdvd_ready_mode="<<iop_reset->cdvd_ready_mode()
                     <<" cdvd_ready_result="<<iop_reset->cdvd_ready_result()
                     <<" boot_device_queries="<<iop_reset->boot_device_queries()
                     <<" boot_device="<<iop_reset->boot_device()
                     <<" boot_device_decision="<<iop_reset->boot_device_decision()
                     <<" boot_device_path="<<iop_reset->boot_device_path()
                     <<" load_decision="<<iop_reset->load_decision()
                     <<" load_bytes="<<iop_reset->load_bytes()
                     <<" load_hash="<<iop_reset->load_hash()
                     <<" load_path="<<iop_reset->load_path()<<'\n';
        }
#endif
        if (!capture_directory.empty()) capture.save(capture_directory);
        if (!stop_directory.empty()) probe::capture_stop(stop_directory,state,memory,stop);
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo<<" syscall="<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write || stop==nt::ee::Stop::unpublished_code || (stop==nt::ee::Stop::unsupported_system && memory.fault_address))
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code) std::cout<<" return_address="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[31].lo));
        if (memory.metrics.rejected) {
            const auto control=memory.metrics.rejected_control;
            std::cout<<" unsupported_performance_control="<<nt::hex32(control)
                     <<" event0="<<((control>>5)&31)<<" event1="<<((control>>15)&31)
                     <<" metric_provider="<<(memory.metrics.request ? "rejected" : "missing")
                     <<" metric_operation="<<static_cast<unsigned>(memory.metrics.rejected_operation)
                     <<" metric_counter="<<static_cast<unsigned>(memory.metrics.rejected_counter);
        }
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x74)
            std::cout<<" requested_syscall="<<state.gpr[4].lo
                     <<" uncompiled_handler="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        if (stop==nt::ee::Stop::unmapped_code && (state.gpr[3].lo==0x20 || state.gpr[3].lo==0x22))
            std::cout<<" uncompiled_thread_entry="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unsupported_system && command_peer.last_command())
            std::cout<<" native_peer_command="<<nt::hex32(command_peer.last_command())<<" option="<<command_peer.last_option();
        if (cdvd && !cdvd->error().empty()) std::cerr<<"disc initialization: "<<cdvd->error()<<'\n';
        if (!gpu.error.empty()) std::cerr<<"graphics: "<<gpu.error<<'\n';
        if (trace_services) {
#ifdef ANYPS2_NATIVE_VU
            std::cout<<" compiled_vu_unit="<<nt_vu_program_unit()<<" compiled_vu_ready="<<compiled_vu.ready();
#endif
#ifdef ANYPS2_NATIVE_VU1
            std::cout<<" compiled_vu1_ready="<<compiled_vu1.ready();
#endif
            if (nt::ee::NativePacketSubmission::maps(memory.fault_address)) {
                const auto base=memory.fault_address&~0xfffU;
                std::cout<<" packet_channel="<<((base-0x10008000)/0x1000)
                         <<" packet_source="<<nt::hex32(platform.packet_submission.read(base+0x10).value)
                         <<" packet_qwords="<<platform.packet_submission.read(base+0x20).value
                         <<" packet_chain="<<nt::hex32(platform.packet_submission.read(base+0x30).value)
                         <<" submissions_enabled="<<platform.packet_submission.policy.enabled()
                         <<" rejected_vif_command="<<platform.packet_setup.rejected_command();
            }
            if (gpu.renderer) std::cout<<" graphics_resets="<<platform.graphics.resets()<<" gpu=\""<<gpu.renderer->device_name()<<"\"";
            if (cdvd) std::cout<<" cdvd_initialized="<<cdvd->initialized();
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
