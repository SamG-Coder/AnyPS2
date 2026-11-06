#pragma once
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_memory_card_files.hpp>
#include <nt/iop_native_modload.hpp>
#include <nt/iop_native_semaphores.hpp>

namespace nt::iop {
// Public MCMAN library calls on native files. The module's SIO2 traffic, card
// authentication and page cache never run. Raw page, block and bad-block calls have
// no meaning for a host file and stop explicitly if a program reaches them, except
// that replacing bad blocks has nothing to do and succeeds. Calls take no console
// time: there is no card to wait for.
class NativeMemoryCardServices final : public NativeServices {
public:
    NativeMemoryCardServices(NativeLibraryServices& libraries,NativeHeapServices& heap,NativeSemaphoreServices& semaphores)
        : libraries_(libraries),heap_(heap),semaphores_(semaphores) {}
    void set_root(std::filesystem::path root) { files_.set_root(std::move(root)); }
    void attach_loader(const NativeModloadServices& loader) { loader_=&loader; }
    const std::string& error() const { return files_.error(); }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::mcman) return {};
        if (request.interrupt_context) return {NativeServiceStatus::unavailable};
        if (request.selector==0x100) return initialize(request,ram);
        if (!registered_) return {NativeServiceStatus::unavailable};
        const auto& a=request.arguments;
        const auto fd=static_cast<int>(a[0]);
        switch (request.selector) {
        case 5: return returned(files_.detect(a[0],a[1],false));
        case 21: return returned(files_.detect(a[0],a[1],true));
        case 22: return returned(files_.format_state(a[0],a[1]));
        case 39: return returned(files_.card_type(a[0],a[1]));
        case 38: return returned(files_.free_clusters(a[0],a[1]));
        case 7: return returned(files_.close(fd));
        case 14: return returned(files_.flush(fd));
        case 25: return returned(files_.close_all());
        case 10: return returned(files_.seek(fd,static_cast<std::int32_t>(a[1]),static_cast<std::int32_t>(a[2])));
        case 11: return returned(files_.format(a[0],a[1]));
        case 24: case 37: case 40: return returned(0);
        case 26: return returned(1); // Slots on a port: there is no multitap here.
        case 42: return returned(static_cast<std::int32_t>(module_info_));
        case 53: return returned(static_cast<std::int32_t>(semaphore_));
        case 6: case 13: case 15: case 16: case 23: case 12: return named(request,ram);
        case 8: case 9: return transfer(request,ram);
        default: return {NativeServiceStatus::unavailable};
        }
    }
private:
    static NativeServiceReply returned(std::int32_t value) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    static NativeServiceReply fault(std::uint32_t address) { return {NativeServiceStatus::address_fault,0,address}; }
    static std::optional<NativeServiceReply> writable(std::uint32_t address,std::uint32_t size,std::span<const std::uint8_t> ram) {
        const auto offset=ram_offset(address,1,ram.size());
        if (!offset || size>ram.size()-*offset) return fault(address);
        for (auto word=*offset&~3U;word<*offset+size;word+=4)
            if (nt_iop_native_code_word(word)) return NativeServiceReply{NativeServiceStatus::code_write,0,address};
        return {};
    }
    static std::optional<std::string> text(std::uint32_t address,std::span<const std::uint8_t> ram) {
        const auto offset=ram_offset(address,1,ram.size());
        if (!offset) return {};
        const auto limit=std::min<std::size_t>(ram.size()-*offset,1024);
        const auto begin=ram.begin()+*offset;
        const auto end=std::find(begin,begin+limit,std::uint8_t(0));
        if (end==begin+limit) return {};
        return std::string(begin,end);
    }
    // The fifth and later arguments of a call are on the caller's stack.
    static std::optional<std::uint32_t> stack_argument(const NativeServiceRequest& request,unsigned index,std::span<const std::uint8_t> ram) {
        const auto offset=ram_offset(request.stack_pointer+16+index*4,4,ram.size());
        if (!offset || ram.size()-*offset<4) return {};
        return load_native_word(ram,*offset);
    }
    NativeServiceReply transfer(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        const auto fd=static_cast<int>(a[0]);
        const auto length=static_cast<std::int32_t>(a[2]);
        if (length<0) return returned(0);
        const auto size=static_cast<std::uint32_t>(length);
        if (request.selector==8 && size) if (const auto refused=writable(a[1],size,ram)) return *refused;
        const auto offset=size ? ram_offset(a[1],1,ram.size()) : std::optional<std::uint32_t>{0U};
        if (!offset || size>ram.size()-*offset) return fault(a[1]);
        const auto bytes=ram.subspan(*offset,size);
        return returned(request.selector==8 ? files_.read(fd,bytes) : files_.write(fd,bytes));
    }
    NativeServiceReply named(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        const auto name=text(a[2],ram);
        if (!name) return fault(a[2]);
        switch (request.selector) {
        case 6: return returned(files_.open(a[0],a[1],*name,a[3]));
        case 13: return returned(files_.remove(a[0],a[1],*name,a[3]));
        case 23: return returned(files_.entry_space(a[0],a[1],*name));
        case 15: {
            if (const auto refused=writable(a[3],1024,ram)) return *refused;
            std::string current;
            const auto result=files_.change_directory(a[0],a[1],*name,current);
            if (!result) {
                const auto offset=*ram_offset(a[3],1,ram.size());
                std::fill_n(ram.begin()+offset,1024,std::uint8_t(0));
                std::copy_n(current.begin(),std::min<std::size_t>(current.size(),1023),ram.begin()+offset);
            }
            return returned(result);
        }
        case 16: {
            const auto flags=stack_argument(request,0,ram);
            if (!flags) return fault(request.stack_pointer);
            const auto offset=ram_offset(a[3],1,ram.size());
            if (!offset || NativeMemoryCardFiles::entry_bytes>ram.size()-*offset) return fault(a[3]);
            return returned(files_.set_info(a[0],a[1],*name,
                ram.subspan(*offset).first<NativeMemoryCardFiles::entry_bytes>(),*flags));
        }
        default: {
            const auto maximum=stack_argument(request,0,ram),output=stack_argument(request,1,ram);
            if (!maximum || !output) return fault(request.stack_pointer);
            if (static_cast<std::int32_t>(*maximum)<0 || *maximum>0x1000) return returned(0);
            const auto bytes=*maximum*static_cast<std::uint32_t>(NativeMemoryCardFiles::entry_bytes);
            if (bytes) if (const auto refused=writable(*output,bytes,ram)) return *refused;
            std::vector<NativeMemoryCardFiles::ListedEntry> listed;
            const auto result=files_.list(a[0],a[1],*name,a[3],*maximum,listed);
            if (result>0) {
                auto target=ram.begin()+*ram_offset(*output,1,ram.size());
                for (const auto& entry:listed) target=std::copy(entry.begin(),entry.end(),target);
            }
            return returned(result);
        }
        }
    }
    NativeServiceReply initialize(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (registered_) return {NativeServiceStatus::unavailable};
        auto declaration=libraries_.declaration_for_entry("mcman",2,request.call_site);
        if (!declaration) declaration=libraries_.declaration_for_entry("mcman",1,request.call_site);
        if (!declaration) return {NativeServiceStatus::unavailable};
        // Two records clients read from guest memory: the semaphore parameters, and the
        // irx_id naming this module, whose version libmc checks before using the card.
        // That is the module's own version, which differs from its export table's.
        constexpr std::uint32_t bytes=32;
        const auto block=heap_.invoke({4,{0,bytes},NativeServiceKind::sysmem},ram);
        if (block.status!=NativeServiceStatus::returned || !block.value) return {NativeServiceStatus::unavailable};
        const auto base=block.value;
        if (base>ram.size() || bytes>ram.size()-base) return fault(base);
        const auto put=[&](std::uint32_t offset,std::uint32_t value) { std::memcpy(ram.data()+base+offset,&value,4); };
        put(0,0); put(4,0); put(8,1); put(12,1);
        const auto created=semaphores_.invoke({4,{base},NativeServiceKind::thsemap},ram);
        if (created.status!=NativeServiceStatus::returned || static_cast<std::int32_t>(created.value)<0)
            return {NativeServiceStatus::unavailable};
        semaphore_=created.value;
        const auto running=loader_ ? loader_->initializing_version() : std::nullopt;
        const auto version=running ? std::uint32_t(*running) : load_native_word(ram,*declaration+8)&65535;
        put(16,base+24); put(20,version);
        std::memcpy(ram.data()+base+24,"mcman",6);
        module_info_=base+16;
        const auto registered=libraries_.invoke({6,{*declaration},NativeServiceKind::loadcore},ram);
        if (registered.status!=NativeServiceStatus::returned || registered.value) return {NativeServiceStatus::unavailable};
        registered_=true;
        return returned(0);
    }
    NativeLibraryServices& libraries_;
    NativeHeapServices& heap_;
    NativeSemaphoreServices& semaphores_;
    const NativeModloadServices* loader_=nullptr;
    NativeMemoryCardFiles files_;
    std::uint32_t semaphore_=0,module_info_=0;
    bool registered_=false;
};
}
