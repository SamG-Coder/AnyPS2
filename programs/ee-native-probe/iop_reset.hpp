#pragma once
#include <nt/ee_iop_delivery.hpp>
#include <nt/ee_iop_commands.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/iop_native_startup.hpp>
#include <nt/iop_llvm.hpp>
#include <nt/binary.hpp>
#include <memory>

nt::iop::NativeModule nt_iop_generated_module();
std::uint64_t nt_iop_generated_data_hash();
std::vector<std::uint64_t> nt_iop_generated_source_hashes();
std::vector<nt::iop::SealedModule> nt_iop_generated_sealed();
std::vector<nt::iop::NativeModule> nt_iop_generated_entries();
std::vector<nt::iop::NativeLibrarySpec> nt_iop_generated_libraries();
std::vector<nt::iop::NativeImportSpec> nt_iop_generated_imports();

namespace probe {
// Opt-in investigation of a linked AOT bundle at the actual EE reset boundary.
// Probe mode stops after startup. Explicit session mode retains the compiled
// workers and services and owns subsequent EE/IOP command delivery.
class IopReset {
    struct Session {
        std::vector<std::uint8_t> ram=std::vector<std::uint8_t>(0x200000);
        std::unique_ptr<nt::iop::NativeServiceContext> services;
        std::unique_ptr<nt::iop::NativeModuleStartup> startup;
        std::unique_ptr<nt::ee::NativeIopCommands> incoming;
        std::unique_ptr<nt::ee::NativeCdvdInitRpc> cdvd;
        std::unique_ptr<nt::ee::NativeCdvdScmdRpc> scmd;
        std::unique_ptr<nt::ee::NativeCdvdReadyRpc> ready,ready_alternate;
        std::unique_ptr<nt::ee::NativeSifCommandPeer> native_rpc;
    };
public:
    IopReset(const std::filesystem::path& data,const std::filesystem::path& disc,
             nt::ee::Memory& ee,nt::ee::NativePlatform& platform,nt::ee::CodeOverlap protection,bool session=false)
        : platform_(platform),module_(nt_iop_generated_module()),session_(session),disc_(disc),data_(data),ee_(ee),protection_(protection),
          delivery_(ee,protection,platform.sif_receive),
          destination_{{ee.data,ee.size},[protection](auto address,auto size) { return !protection(address,size); }} {
        state_=prepare();
        platform_.graphics.edge_context=this;
        platform_.graphics.display_edge=[](void* context,bool end) {
            auto& self=*static_cast<IopReset*>(context);
            if (self.accepted_ && self.started_) self.state_->services->vblank.post(end);
        };
        platform_.graphics.display_overflow=[](void* context) {
            auto& self=*static_cast<IopReset*>(context);
            if (self.accepted_ && self.started_) self.state_->services->vblank.overflow();
        };
    }
    nt::ee::NativeSifResetOwner owner() { return {this,reset,{this,commands}}; }
    ~IopReset() {
        if (platform_.graphics.edge_context==this) {
            platform_.graphics.display_edge=nullptr; platform_.graphics.display_overflow=nullptr; platform_.graphics.edge_context=nullptr;
        }
    }
    bool invoked() const { return invoked_; }
    nt::iop::NativeResult result() const { return result_; }
    std::span<const std::uint8_t> data() const { return state_->ram; }
    const nt::iop::Cpu* active_state() const { return state_->services->threads.active_state(); }
    std::uint64_t executed() const { return retired_lifetimes_+state_->startup->executed(); }
    std::uint32_t generations() const { return generations_; }
    std::size_t returned() const { return state_->startup->returned().size(); }
    bool finished() const { return state_->startup->finished(); }
    bool accepted() const { return accepted_; }
    std::uint32_t last_server() const { return last_server_; }
    bool cdvd_initialized() const { return state_->cdvd && state_->cdvd->initialized(); }
    const nt::iop::NativeSerialServices& serial() const { return state_->services->serial(); }
    const nt::iop::NativeUsbServices& usb() const { return state_->services->usb; }
    const nt::audio::Output& audio() const { return *state_->services->audio_output; }
    const auto& rejected_transfer() const { return state_->services->sif.rejected_transfer(); }
    std::uint32_t cdvd_command() const { return state_->scmd ? state_->scmd->last_method() : 0; }
    std::uint32_t cdvd_media() const { return state_->cdvd ? state_->cdvd->media_mode() : 0; }
    std::uint32_t cdvd_argument() const { return state_->scmd ? state_->scmd->last_argument() : 0; }
    std::uint32_t cdvd_ready_calls() const {
        return (state_->ready ? state_->ready->calls() : 0)+(state_->ready_alternate ? state_->ready_alternate->calls() : 0);
    }
    std::uint32_t cdvd_ready_mode() const {
        if (state_->ready_alternate && state_->ready_alternate->calls()) return state_->ready_alternate->last_mode();
        return state_->ready ? state_->ready->last_mode() : 0;
    }
    std::uint32_t cdvd_ready_result() const {
        if (state_->ready_alternate && state_->ready_alternate->calls()) return state_->ready_alternate->last_result();
        return state_->ready ? state_->ready->last_result() : 0;
    }
    std::uint32_t boot_device_queries() const { return state_->services->modload.boot_device_queries(); }
    const std::string& boot_device() const { return state_->services->modload.boot_device(); }
    const std::string& boot_device_path() const { return state_->services->modload.boot_device_path(); }
    int boot_device_decision() const { return state_->services->modload.boot_device_decision(); }
    int load_decision() const { return state_->services->modload.load_decision(); }
    const std::string& load_path() const { return state_->services->modload.load_path(); }
    std::uint64_t load_bytes() const { return state_->services->modload.load_bytes(); }
    std::uint64_t load_hash() const { return state_->services->modload.load_hash(); }
    nt::ee::Stop pump(std::uint64_t work_budget=1000000) {
        if (!accepted_) return nt::ee::Stop::budget;
        if (!platform_.sif_link.reset_teardown_complete()) return nt::ee::Stop::budget;
        if (!started_) {
            start(work_budget);
            if (started_)
                platform_.sif_link.replace_peer(state_->services->sif_control.sub_address(),state_->services->sif_control.sub_flags());
            return result_.reason==nt::iop::NativeStop::budget || (started_ && result_.reason==nt::iop::NativeStop::thread_wait) ?
                nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
        }
        auto& control=state_->services->sif_control;
        auto& link=platform_.sif_link;
        control.acknowledge_sub(control.sub_flags() & ~link.peer_flags());
        control.publish_main(link.main_address(),link.main_flags());
        result_=state_->services->threads.run(nt::iop::run_llvm,state_->ram,work_budget,*state_->services,
            nt::iop::NativeThreadServices::CompletionPolicy::continue_workers);
        link.acknowledge_main(link.main_flags() & ~control.main_flags());
        link.publish_peer(control.sub_address(),control.sub_flags());
        return result_.reason==nt::iop::NativeStop::thread_wait || result_.reason==nt::iop::NativeStop::budget ?
            nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
    }
private:
    std::unique_ptr<Session> prepare() {
        const auto& data=data_;
        const auto& disc=disc_;
        auto& ee=ee_;
        auto& platform=platform_;
        const auto protection=protection_;
        auto next=std::make_unique<Session>();
        const auto bytes=nt::read_file(data,0x200000);
        if (module_.base>=0x1f0000 || module_.memory_size>0x1f0000-module_.base ||
            module_.initialized_size>module_.memory_size || bytes.size()!=module_.initialized_size ||
            nt::fingerprint(bytes)!=nt_iop_generated_data_hash())
            throw std::runtime_error("Invalid linked IOP startup data");
        std::copy(bytes.begin(),bytes.end(),next->ram.begin()+module_.base);
        next->services=std::make_unique<nt::iop::NativeServiceContext>(next->ram,
            nt::iop::NativeContextLayout{(module_.base+module_.memory_size+255)&~255U,0x1ed000},
            nt_iop_generated_libraries(),nt_iop_generated_imports(),
            [](std::string_view) { return true; },disc,&destination_,0,delivery_);
        next->services->modload.attach_catalog(nt_iop_generated_source_hashes());
        install_sealed(data,*next);
        next->startup=std::make_unique<nt::iop::NativeModuleStartup>(nt::iop::run_llvm,*next->services,next->ram,module_,nt_iop_generated_entries(),0x1ff000);
        next->incoming=std::make_unique<nt::ee::NativeIopCommands>(next->services->sif_control,next->services->inbound);
        if (!disc.empty()) {
            next->cdvd=std::make_unique<nt::ee::NativeCdvdInitRpc>(disc,0x1ee000,0x1ee010);
            next->scmd=std::make_unique<nt::ee::NativeCdvdScmdRpc>(*next->cdvd,0x1ed000,0x1ed010);
            next->ready=std::make_unique<nt::ee::NativeCdvdReadyRpc>(*next->cdvd,0x8000059AU,0x1ef000,0x1ef010);
            next->ready_alternate=std::make_unique<nt::ee::NativeCdvdReadyRpc>(*next->cdvd,0x8000059CU,0x1ef800,0x1ef810);
            next->native_rpc=std::make_unique<nt::ee::NativeSifCommandPeer>(platform.sif_link,ee,protection,0x1f0000,&platform.sif_receive);
            if (!next->native_rpc->register_service(*next->cdvd)) throw std::runtime_error("Cannot register reset-session CD/DVD service");
            if (!next->native_rpc->register_service(*next->scmd)) throw std::runtime_error("Cannot register reset-session CD/DVD command service");
            if (!next->native_rpc->register_service(*next->ready) || !next->native_rpc->register_service(*next->ready_alternate))
                throw std::runtime_error("Cannot register reset-session CD/DVD ready service");
        }
        return next;
    }
    // An empty record list does not read sealed.data. Records are installed at
    // their converted bases; a base that does not already fit is refused.
    void install_sealed(const std::filesystem::path& data,Session& next) {
        const auto sealed=nt_iop_generated_sealed();
        if (sealed.empty()) return;
        const auto sidecar=data.parent_path()/"sealed.data";
        if (!std::filesystem::is_regular_file(sidecar)) throw std::runtime_error("Sealed module image is missing");
        const auto bytes=nt::read_file(sidecar,0x200000);
        const auto heap_begin=(module_.base+module_.memory_size+255U)&~255U;
        constexpr std::uint32_t heap_limit=0x1ed000;
        const auto image_begin=std::uint64_t(module_.base);
        const auto image_end=image_begin+module_.memory_size;
        std::size_t cursor=0;
        for (std::size_t index=0;index<sealed.size();++index) {
            const auto& record=sealed[index];
            if (record.base&255U) throw std::runtime_error("Sealed module base is not 256-byte aligned");
            const auto rounded=(std::uint64_t(record.memory_size)+255U)&~std::uint64_t{255};
            const auto span_end=std::uint64_t(record.base)+rounded;
            if (!record.memory_size || record.initialized_size>record.memory_size || record.base<heap_begin || span_end>heap_limit)
                throw std::runtime_error("Sealed module is outside the native heap and was not relocated");
            if (record.base<image_end && image_begin<span_end)
                throw std::runtime_error("Sealed module intersects the startup image and was not relocated");
            for (std::size_t prior=0;prior<index;++prior) {
                const auto& other=sealed[prior];
                const auto other_end=std::uint64_t(other.base)+((std::uint64_t(other.memory_size)+255U)&~std::uint64_t{255});
                if (record.base<other_end && std::uint64_t(other.base)<span_end)
                    throw std::runtime_error("Sealed modules overlap and were not relocated");
            }
            if (cursor>bytes.size() || record.initialized_size>bytes.size()-cursor)
                throw std::runtime_error("Sealed module image is short");
            const auto image=nt::ByteView(bytes).subspan(cursor,record.initialized_size);
            if (nt::fingerprint(image)!=record.image_hash) throw std::runtime_error("Sealed module image hash mismatch");
            if (std::uint64_t(record.base)+record.initialized_size>next.ram.size())
                throw std::runtime_error("Sealed module image exceeds IOP RAM");
            std::copy(image.begin(),image.end(),next.ram.begin()+record.base);
            if (!next.services->heap.hold(record.base,record.memory_size))
                throw std::runtime_error("Sealed module overlaps a reserved heap span");
            cursor+=record.initialized_size;
        }
        next.services->modload.attach_sealed(sealed);
    }
    void start(std::uint64_t work_budget=1000000) {
        state_->services->sif_control.publish_main(platform_.sif_link.main_address(),platform_.sif_link.main_flags());
        result_=state_->startup->advance(work_budget);
        if (!state_->startup->finished()) return;
        const auto initialized=state_->services->initialize_commands();
        if (initialized.status!=nt::iop::NativeServiceStatus::returned) {
            result_={nt::iop::NativeStop::import_request,0,0,0x70005}; return;
        }
        started_=true;
        state_->services->interrupts.invoke({9,{},nt::iop::NativeServiceKind::intrman},state_->ram);
        result_=state_->services->threads.run(nt::iop::run_llvm,state_->ram,work_budget,*state_->services,
            nt::iop::NativeThreadServices::CompletionPolicy::continue_workers);
    }
    static nt::ee::IoRead commands(void* opaque,std::span<const nt::ee::NativeSifPacket> packets) {
        auto& self=*static_cast<IopReset*>(opaque);
        if (!self.accepted_ || !self.started_) return {nt::ee::Stop::unsupported_system};
        nt::ee::IoRead result;
        if (self.state_->native_rpc && self.state_->native_rpc->owns_rpc(packets)) {
            if (self.state_->services->inbound.busy()) return {nt::ee::Stop::budget,0};
            result=self.state_->native_rpc->dispatch_rpc(packets,self.state_->services->endpoint()->command_buffer(),self.state_->services->command_session.rpc_ready());
        } else {
            const auto sink=self.state_->incoming->sink();
            result=sink.accept(sink.context,packets);
        }
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
        if (self.session_) {
            if (self.accepted_ && !self.started_) return {nt::ee::Stop::budget,0};
            if (self.platform_.sif_receive.accepting() || self.generations_==0xffffffffU)
                return {nt::ee::Stop::unsupported_system};
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
            // Allocate and validate before retiring the old serialized lifetime.
            // Its workers, handles, callbacks and pending replies are destroyed
            // together; no old owner can deliver into the replacement session.
            if (self.invoked_) {
                auto next=self.prepare();
                self.retired_lifetimes_+=self.state_->startup->executed();
                self.state_=std::move(next);
            }
            self.invoked_=true; self.accepted_=true; self.started_=false;
            self.last_server_=0; self.result_={nt::iop::NativeStop::budget,0};
            ++self.generations_;
            self.platform_.sif_link.begin_peer_reset();
            return {nt::ee::Stop::budget,1};
        }
        if (!self.invoked_) { self.invoked_=true; self.start(); }
        return {nt::ee::Stop::unsupported_system};
    }
    nt::ee::NativePlatform& platform_;
    nt::iop::NativeModule module_;
    bool session_=false,accepted_=false;
    std::filesystem::path disc_,data_;
    nt::ee::Memory& ee_;
    nt::ee::CodeOverlap protection_;
    nt::ee::NativeIopDelivery delivery_;
    nt::iop::NativeSifDestination destination_;
    std::unique_ptr<Session> state_;
    nt::iop::NativeResult result_{nt::iop::NativeStop::budget,0};
    bool invoked_=false,started_=false;
    std::uint32_t last_server_=0;
    std::uint32_t generations_=0;
    std::uint64_t retired_lifetimes_=0;
};
}
