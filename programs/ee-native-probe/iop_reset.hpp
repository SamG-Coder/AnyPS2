#pragma once
#include <nt/ee_iop_delivery.hpp>
#include <nt/ee_iop_commands.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/iop_native_startup.hpp>
#include <nt/iop_llvm.hpp>
#include <nt/binary.hpp>
#include <memory>

nt::iop::NativeModule nt_iop_generated_module();
std::uint64_t nt_iop_generated_data_hash();
std::vector<std::uint64_t> nt_iop_generated_source_hashes();
std::vector<nt::iop::NativeModule> nt_iop_generated_entries();
std::vector<nt::iop::NativeLibrarySpec> nt_iop_generated_libraries();
std::vector<nt::iop::NativeImportSpec> nt_iop_generated_imports();

namespace probe {
// Opt-in investigation of a linked AOT bundle at the actual EE reset boundary.
// Probe mode stops after startup. Explicit session mode retains the compiled
// workers and services and owns subsequent EE/IOP command delivery.
class IopReset {
public:
    IopReset(const std::filesystem::path& data,const std::filesystem::path& disc,
             nt::ee::Memory& ee,nt::ee::NativePlatform& platform,nt::ee::CodeOverlap protection,bool session=false)
        : platform_(platform),module_(nt_iop_generated_module()),ram_(0x200000),session_(session),disc_(disc),
          delivery_(ee,protection,platform.sif_receive),
          destination_{{ee.data,ee.size},[protection](auto address,auto size) { return !protection(address,size); }} {
        const auto bytes=nt::read_file(data,0x200000);
        if (module_.base>=0x1f0000 || module_.memory_size>0x1f0000-module_.base ||
            module_.initialized_size>module_.memory_size || bytes.size()!=module_.initialized_size ||
            nt::fingerprint(bytes)!=nt_iop_generated_data_hash())
            throw std::runtime_error("Invalid linked IOP startup data");
        std::copy(bytes.begin(),bytes.end(),ram_.begin()+module_.base);
        services_=std::make_unique<nt::iop::NativeServiceContext>(ram_,
            nt::iop::NativeContextLayout{(module_.base+module_.memory_size+255)&~255U},
            nt_iop_generated_libraries(),nt_iop_generated_imports(),
            [](std::string_view) { return true; },disc,&destination_,0,delivery_);
        startup_=std::make_unique<nt::iop::NativeModuleStartup>(nt::iop::run_llvm,*services_,ram_,module_,nt_iop_generated_entries(),0x1ff000);
        incoming_=std::make_unique<nt::ee::NativeIopCommands>(services_->sif_control,services_->inbound);
    }
    nt::ee::NativeSifResetOwner owner() { return {this,reset,{this,commands}}; }
    bool invoked() const { return invoked_; }
    nt::iop::NativeResult result() const { return result_; }
    std::uint64_t executed() const { return startup_->executed(); }
    std::size_t returned() const { return startup_->returned().size(); }
    bool finished() const { return startup_->finished(); }
    bool accepted() const { return accepted_; }
    std::uint32_t last_server() const { return last_server_; }
    nt::ee::Stop pump() {
        if (!accepted_) return nt::ee::Stop::budget;
        if (!platform_.sif_link.reset_teardown_complete()) return nt::ee::Stop::budget;
        if (!started_) {
            start();
            platform_.sif_link.replace_peer(services_->sif_control.sub_address(),services_->sif_control.sub_flags());
            return startup_->finished() && result_.reason==nt::iop::NativeStop::thread_wait ?
                nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
        }
        auto& control=services_->sif_control;
        auto& link=platform_.sif_link;
        control.acknowledge_sub(control.sub_flags() & ~link.peer_flags());
        control.publish_main(link.main_address(),link.main_flags());
        result_=services_->threads.run(nt::iop::run_llvm,ram_,1000000,*services_);
        link.acknowledge_main(link.main_flags() & ~control.main_flags());
        link.publish_peer(control.sub_address(),control.sub_flags());
        return result_.reason==nt::iop::NativeStop::thread_wait ? nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
    }
private:
    void start() {
        started_=true;
        services_->sif_control.publish_main(platform_.sif_link.main_address(),platform_.sif_link.main_flags());
        result_=startup_->advance(1000000);
        if (!startup_->finished()) return;
        const auto initialized=services_->initialize_commands();
        if (initialized.status!=nt::iop::NativeServiceStatus::returned) {
            result_={nt::iop::NativeStop::import_request,0,0,0x70005}; return;
        }
        services_->interrupts.invoke({9,{},nt::iop::NativeServiceKind::intrman},ram_);
        result_=services_->threads.run(nt::iop::run_llvm,ram_,1000000,*services_);
    }
    static nt::ee::IoRead commands(void* opaque,std::span<const nt::ee::NativeSifPacket> packets) {
        auto& self=*static_cast<IopReset*>(opaque);
        if (!self.accepted_) return {nt::ee::Stop::unsupported_system};
        const auto sink=self.incoming_->sink();
        const auto result=sink.accept(sink.context,packets);
        if (result.status==nt::ee::Stop::budget && result.value==1 && !packets.empty()) {
            const auto& bytes=packets.back().bytes;
            if (bytes.size()>=36 && nt::iop::load_native_word(bytes,8)==0x80000009)
                self.last_server_=nt::iop::load_native_word(bytes,32);
        }
        return result;
    }
    static nt::ee::IoRead reset(void* opaque,const nt::ee::NativeSifResetRequest& request) {
        auto& self=*static_cast<IopReset*>(opaque);
        if (request.mode) return {nt::ee::Stop::unsupported_system};
        if (!self.invoked_) {
            if (self.session_) {
                constexpr std::string_view loader="rom0:UDNL ";
                if (!request.arguments.starts_with(loader) || self.disc_.empty()) return {nt::ee::Stop::unsupported_system};
                nt::disc::Files disc(self.disc_);
                const auto image=disc.read_file(std::string(request.arguments.substr(loader.size())),16*1024*1024);
                const auto files=nt::disc::parse_reboot_image(image);
                const auto sources=nt_iop_generated_source_hashes();
                if (sources.empty()) return {nt::ee::Stop::unsupported_system};
                for (const auto source:sources) {
                    const auto match=std::find_if(files.begin(),files.end(),[&](const auto& file) {
                        return nt::fingerprint(nt::ByteView(image).subspan(file.offset,file.size))==source;
                    });
                    if (match==files.end()) return {nt::ee::Stop::unsupported_system};
                }
            }
            self.invoked_=true;
            if (self.session_) {
                // Accept the prepared reset job, not completed initialization.
                // EE teardown clears the old SIFINIT/CMDINIT announcements before
                // the new session may publish its independently produced flags.
                self.platform_.sif_link.begin_peer_reset();
                self.accepted_=true;
                return {nt::ee::Stop::budget,1};
            }
            self.start();
        }
        return {nt::ee::Stop::unsupported_system};
    }
    nt::ee::NativePlatform& platform_;
    nt::iop::NativeModule module_;
    std::vector<std::uint8_t> ram_;
    bool session_=false,accepted_=false;
    std::filesystem::path disc_;
    nt::ee::NativeIopDelivery delivery_;
    nt::iop::NativeSifDestination destination_;
    std::unique_ptr<nt::iop::NativeServiceContext> services_;
    std::unique_ptr<nt::iop::NativeModuleStartup> startup_;
    std::unique_ptr<nt::ee::NativeIopCommands> incoming_;
    nt::iop::NativeResult result_{nt::iop::NativeStop::budget,0};
    bool invoked_=false,started_=false;
    std::uint32_t last_server_=0;
};
}
