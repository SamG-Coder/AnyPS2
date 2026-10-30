#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <algorithm>
#include <atomic>
#include <string>

namespace nt::iop {
// Native loader ABI ownership. Callback registration does not load a module,
// authenticate executable content, or mark any module resident.
class NativeModloadServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::loadcore && (request.selector==4 || request.selector==5)) {
            // AOT instructions are immutable and native data accesses are coherent.
            // Preserve the publication ordering without a simulated cache bank.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return {NativeServiceStatus::returned};
        }
        if (request.kind!=NativeServiceKind::modload) return {};
        if (request.interrupt_context) return {NativeServiceStatus::unavailable};
        const auto& a=request.arguments;
        if (request.selector==12 || request.selector==13) {
            const auto count=request.selector==12 ? 3U : 1U;
            for (unsigned index=0;index<count;++index) {
                if (!a[index]) continue;
                const auto offset=ram_offset(a[index],4,ram.size());
                if (!offset || !nt_iop_native_instruction(*offset))
                    return {NativeServiceStatus::unmapped_code,0,a[index]};
            }
            if (request.selector==12) {
                card_=a[0]; disc_=a[1]; callbacks_=a[2];
            } else path_=a[0];
            return {NativeServiceStatus::returned};
        }
        if (request.selector==14) {
            std::array<std::uint32_t,2> outputs{};
            for (unsigned index=0;index<outputs.size();++index) {
                const auto at=ram_offset(a[index],4,ram.size());
                if (!at || ram.size()-*at<4 || 0x200000-*at<4)
                    return {NativeServiceStatus::address_fault,0,a[index]};
                if (nt_iop_native_code_word(*at)) return {NativeServiceStatus::code_write,0,a[index]};
                outputs[index]=*at;
            }
            store_native_word(ram,outputs[0],path_);
            store_native_word(ram,outputs[1],callbacks_);
            return {NativeServiceStatus::returned};
        }
        if (request.selector==15) return boot_device(a[0],ram);
        // Known lifecycle ABIs can be compiled, but cannot succeed until their
        // native catalog, initialization and completion owner is connected.
        return {NativeServiceStatus::unavailable};
    }
    std::uint32_t boot_device_queries() const { return boot_queries_; }
    const std::string& boot_device() const { return boot_device_; }
    const std::string& boot_device_path() const { return boot_path_; }
    int boot_device_decision() const { return boot_decision_; }
private:
    // Selector 15 names a device class. It does not load or authenticate a module.
    // cdrom: and cdrom0: match the disc opener. rom: through rom3: match the archive
    // unit range and do not require a mounted image. Every other path is illegal.
    static NativeServiceReply guest_text(std::uint32_t address,std::span<const std::uint8_t> ram,std::string& text) {
        const auto start=ram_offset(address,1,ram.size());
        if (!start || !*start) return {NativeServiceStatus::address_fault,0,address};
        const auto available=std::min<std::size_t>({256,ram.size()-*start,0x200000-*start});
        const auto bytes=ram.subspan(*start,available);
        const auto end=std::find(bytes.begin(),bytes.end(),0);
        if (end==bytes.end())
            return available==256 ? NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(-36)}
                                  : NativeServiceReply{NativeServiceStatus::address_fault,0,address};
        text.assign(bytes.begin(),end);
        return {NativeServiceStatus::returned};
    }
    static std::string device_class(std::string_view path) {
        const auto colon=path.find(':');
        if (colon==std::string_view::npos || !colon) return {};
        auto end=colon;
        while (end && path[end-1]>='0' && path[end-1]<='9') --end;
        if (!end || end>32) return {};
        return std::string(path.substr(0,end));
    }
    static bool legal_boot_device(std::string_view path) {
        if (path.starts_with("cdrom0:") || path.starts_with("cdrom:")) return true;
        if (!path.starts_with("rom")) return false;
        const auto colon=path.find(':');
        if (colon==std::string_view::npos || colon<3) return false;
        unsigned unit=0;
        for (std::size_t at=3;at<colon;++at) {
            if (path[at]<'0' || path[at]>'9') return false;
            if (unit>3) return false;
            unit=unit*10u+static_cast<unsigned>(path[at]-'0');
        }
        return unit<4;
    }
    NativeServiceReply boot_device(std::uint32_t address,std::span<const std::uint8_t> ram) {
        ++boot_queries_;
        boot_device_.clear();
        boot_path_.clear();
        boot_decision_=-1;
        std::string path;
        const auto text=guest_text(address,ram,path);
        if (text.status!=NativeServiceStatus::returned || text.value) return text;
        boot_path_=path;
        boot_device_=device_class(path);
        boot_decision_=legal_boot_device(path)?0:1;
        return {NativeServiceStatus::returned,static_cast<std::uint32_t>(boot_decision_)};
    }
    std::uint32_t card_=0,disc_=0,path_=0,callbacks_=0;
    std::uint32_t boot_queries_=0;
    int boot_decision_=-1;
    std::string boot_device_;
    std::string boot_path_;
};
}
