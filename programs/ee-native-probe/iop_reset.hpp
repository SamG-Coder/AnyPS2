#pragma once
#include <nt/ee_iop_delivery.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/iop_native_startup.hpp>
#include <nt/iop_llvm.hpp>
#include <nt/binary.hpp>
#include <memory>

nt::iop::NativeModule nt_iop_generated_module();
std::uint64_t nt_iop_generated_data_hash();
std::vector<nt::iop::NativeModule> nt_iop_generated_entries();
std::vector<nt::iop::NativeLibrarySpec> nt_iop_generated_libraries();
std::vector<nt::iop::NativeImportSpec> nt_iop_generated_imports();

namespace probe {
// Opt-in investigation of a linked AOT bundle at the actual EE reset boundary.
// Startup completion alone is insufficient to accept reset. This probe always
// stops, retaining its state for diagnostics instead of claiming a booted IOP.
class IopReset {
public:
    IopReset(const std::filesystem::path& data,const std::filesystem::path& disc,
             nt::ee::Memory& ee,nt::ee::NativePlatform& platform,nt::ee::CodeOverlap protection)
        : platform_(platform),module_(nt_iop_generated_module()),ram_(0x200000),
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
    }
    nt::ee::NativeSifResetOwner owner() { return {this,reset,{this,commands}}; }
    bool invoked() const { return invoked_; }
    nt::iop::NativeResult result() const { return result_; }
    std::uint64_t executed() const { return startup_->executed(); }
    std::size_t returned() const { return startup_->returned().size(); }
    bool finished() const { return startup_->finished(); }
private:
    static nt::ee::IoRead commands(void*,std::span<const nt::ee::NativeSifPacket>) { return {nt::ee::Stop::unsupported_system}; }
    static nt::ee::IoRead reset(void* opaque,const nt::ee::NativeSifResetRequest&) {
        auto& self=*static_cast<IopReset*>(opaque);
        if (!self.invoked_) {
            self.invoked_=true;
            self.services_->sif_control.publish_main(self.platform_.sif_link.main_address(),self.platform_.sif_link.main_flags());
            // Module entry execution does not require a completed EE handshake.
            // Its real services retain all initialization and callback state.
            self.result_=self.startup_->advance(1000000);
        }
        return {nt::ee::Stop::unsupported_system};
    }
    nt::ee::NativePlatform& platform_;
    nt::iop::NativeModule module_;
    std::vector<std::uint8_t> ram_;
    nt::ee::NativeIopDelivery delivery_;
    nt::iop::NativeSifDestination destination_;
    std::unique_ptr<nt::iop::NativeServiceContext> services_;
    std::unique_ptr<nt::iop::NativeModuleStartup> startup_;
    nt::iop::NativeResult result_{nt::iop::NativeStop::budget,0};
    bool invoked_=false;
};
}
