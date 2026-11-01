#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_file_descriptors.hpp>
#include <nt/reboot_image.hpp>
#include <algorithm>

namespace nt::iop {
// Read-only ROM filesystem ABI backed exclusively by caller-supplied disc
// archives. No firmware address space, synthesized files or implicit mounts.
class NativeArchiveServices final : public NativeServices {
public:
    explicit NativeArchiveServices(NativeFileDescriptors& descriptors):descriptors_(descriptors) {}
    ~NativeArchiveServices() override {
        for (const auto& handle:handles_) if (handle) descriptors_.release(handle->descriptor,this);
    }
    NativeArchiveServices(const NativeArchiveServices&)=delete;
    NativeArchiveServices& operator=(const NativeArchiveServices&)=delete;
    bool initialize() { if (active_) return false; active_=true; return true; }
    bool initialized() const { return active_; }
    bool mount(unsigned unit,Bytes image) {
        if (!active_ || unit>=images_.size() || images_[unit]) return false;
        auto files=disc::parse_reboot_image(image);
        images_[unit]=Image{std::move(image),std::move(files)};
        return true;
    }
    bool unmount(unsigned unit) {
        if (unit>=images_.size() || !images_[unit]) return false;
        for (const auto& handle:handles_) if (handle && handle->unit==unit) return false;
        images_[unit].reset(); return true;
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (!active_) return {};
        if (request.kind==NativeServiceKind::romdrv) {
            if (request.interrupt_context) return {NativeServiceStatus::unavailable};
            const auto unit=request.arguments[0];
            if (request.selector==5) return returned(unmount(unit) ? 0 : -161);
            if (request.selector!=4) return {NativeServiceStatus::unavailable};
            if (unit>=images_.size() || images_[unit]) return returned(-160);
            const auto start=ram_offset(request.arguments[1],4,ram.size());
            if (!start || !*start) return returned(-162);
            try {
                const auto bytes=std::span<const std::uint8_t>(ram).subspan(*start,std::min(ram.size()-*start,std::size_t(0x200000-*start)));
                const auto files=disc::parse_reboot_image(bytes);
                std::size_t size=0;
                for (const auto& file:files) size=std::max(size,file.offset+file.size);
                return returned(mount(unit,Bytes(bytes.begin(),bytes.begin()+size)) ? 0 : -160);
            } catch (const std::runtime_error&) { return returned(-162); }
        }
        if (request.kind!=NativeServiceKind::ioman) return {};
        const auto& a=request.arguments;
        if (request.selector==4) return open(a[0],a[1],ram);
        const auto descriptor=descriptors_.find(a[0]);
        if (!descriptor || descriptor->owner!=this) return {};
        if (descriptor->local>=handles_.size() || !handles_[descriptor->local]) return {NativeServiceStatus::unavailable};
        auto& handle=*handles_[descriptor->local];
        const auto& image=*images_[handle.unit];
        const auto& file=image.files[handle.file];
        if (request.selector==5) {
            handles_[descriptor->local].reset(); descriptors_.release(a[0],this); return returned(0);
        }
        if (request.selector==7) return returned(-9);
        if (request.selector==8) {
            if (a[2]>2) return returned(-22);
            // ROMDRV's public ABI clamps unsigned wrapped offsets to EOF.
            const auto base=a[2]==0 ? 0U : a[2]==1 ? handle.cursor : static_cast<std::uint32_t>(file.size);
            handle.cursor=std::min(base+a[1],static_cast<std::uint32_t>(file.size));
            return returned(handle.cursor);
        }
        if (request.selector!=6) return {NativeServiceStatus::unavailable};
        if (a[2]>0x7fffffffU) return returned(-22);
        const auto count=std::min<std::size_t>(a[2],file.size-handle.cursor);
        if (!count) return returned(0);
        const auto output=ram_offset(a[1],1,ram.size());
        if (!output || count>ram.size()-*output || count>0x200000-*output)
            return {NativeServiceStatus::address_fault,0,a[1]};
        for (auto word=*output&~3U;word<*output+count;word+=4)
            if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,a[1]};
        std::copy_n(image.bytes.begin()+file.offset+handle.cursor,count,ram.begin()+*output);
        handle.cursor+=static_cast<std::uint32_t>(count);
        return returned(static_cast<std::uint32_t>(count));
    }
private:
    static NativeServiceReply returned(std::uint32_t value) { return {NativeServiceStatus::returned,value}; }
    NativeServiceReply open(std::uint32_t address,std::uint32_t mode,std::span<const std::uint8_t> ram) {
        const auto start=ram_offset(address,1,ram.size());
        if (!start) return {NativeServiceStatus::address_fault,0,address};
        const auto available=std::min<std::size_t>({256,ram.size()-*start,0x200000-*start});
        const auto bytes=ram.subspan(*start,available);
        const auto end=std::find(bytes.begin(),bytes.end(),0);
        if (end==bytes.end()) return available==256 ? returned(-36) : NativeServiceReply{NativeServiceStatus::address_fault,0,address};
        const std::string path(bytes.begin(),end);
        if (!path.starts_with("rom")) return {};
        const auto colon=path.find(':');
        if (colon==std::string::npos || colon<3) return {};
        unsigned unit=0;
        for (std::size_t at=3;at<colon;++at) {
            if (path[at]<'0' || path[at]>'9') return {};
            if (unit>3) return returned(-6);
            unit=unit*10+static_cast<unsigned>(path[at]-'0');
        }
        if (unit>=images_.size() || !images_[unit]) return returned(-6);
        if (mode!=1) return returned(-13);
        const auto& files=images_[unit]->files;
        const auto found=std::find_if(files.begin(),files.end(),[&](const auto& file) { return file.name==path.substr(colon+1); });
        if (found==files.end()) return returned(-2);
        const auto slot=std::find(handles_.begin(),handles_.end(),std::nullopt);
        if (slot==handles_.end()) return returned(-12);
        const auto index=static_cast<std::uint32_t>(slot-handles_.begin());
        const auto fd=descriptors_.allocate(this,index);
        if (!fd) return returned(-24);
        *slot=Handle{unit,static_cast<std::size_t>(found-files.begin()),0,*fd};
        return returned(*fd);
    }
    struct Image { Bytes bytes; std::vector<disc::RebootFile> files; };
    struct Handle { unsigned unit; std::size_t file; std::uint32_t cursor,descriptor; };
    NativeFileDescriptors& descriptors_;
    std::array<std::optional<Image>,4> images_;
    std::array<std::optional<Handle>,8> handles_;
    bool active_=false;
};
}
