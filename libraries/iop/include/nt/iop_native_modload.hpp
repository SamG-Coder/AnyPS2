#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_heap.hpp>
#include <nt/binary.hpp>
#include <nt/disc_files.hpp>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nt::iop {
// One ahead-of-time module the loader may start. source_hash identifies the
// opened file. image_hash identifies the relocated bytes already installed at
// base. The loader does not relocate the file or interpret its instructions.
struct SealedModule {
    std::uint64_t source_hash=0;
    std::uint64_t image_hash=0;
    std::uint32_t base=0, entry=0, initialized_size=0, memory_size=0, global_pointer=0;
};
// Native loader ABI ownership. Callback registration does not load a module,
// authenticate executable content, or mark any module resident.
class NativeModloadServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) return finish_load(request,ram);
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
        if (request.selector==7) return load_start(request,ram);
        if (request.selector==8) return start_module(a[0]);
        // Other lifecycle ABIs can be compiled, but cannot succeed until their
        // native initialization and completion owner is connected.
        return {NativeServiceStatus::unavailable};
    }
    void attach_disc(const std::filesystem::path& image) { files_=std::make_unique<disc::Files>(image); }
    void attach_heap(NativeHeapServices& heap) { heap_=&heap; }
    void attach_catalog(std::vector<std::uint64_t> source_hashes) { catalog_=std::move(source_hashes); }
    void attach_sealed(std::vector<SealedModule> modules) { sealed_=std::move(modules); }
    // -1 means selector 7 did not classify a file. 14 and 15 are StartModule
    // results for an already started id and an unknown id. They are diagnostics,
    // not a module id and not evidence that an entry ran.
    int load_decision() const { return load_decision_; }
    const std::string& load_path() const { return load_path_; }
    std::uint64_t load_bytes() const { return load_bytes_; }
    std::uint64_t load_hash() const { return load_hash_; }
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
    // Ordinal 7 opens a cdrom module image. A hash without a sealed entry does
    // not relocate, register, or call anything. A sealed entry runs only when
    // its relocated image is already installed and its address is compiled code.
    NativeServiceReply load_start(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        // A null filename is an illegal object. It is not an address fault and
        // it does not open a file. The module id is not this error code.
        if (!a[0]) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-201)};
        std::string path;
        const auto text=guest_text(a[0],ram,path);
        if (text.status!=NativeServiceStatus::returned || text.value) return text;
        if (a[1]>65536) return {NativeServiceStatus::unavailable};
        if (a[1]) {
            const auto args=ram_offset(a[2],1,ram.size());
            if (!args || ram.size()-*args<a[1] || 0x200000-*args<a[1])
                return {NativeServiceStatus::address_fault,0,a[2]};
            const auto bytes=ram.subspan(*args,a[1]);
            for (std::size_t at=0;at<bytes.size();) {
                const auto end=std::find(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.end(),0);
                if (end==bytes.end()) return {NativeServiceStatus::address_fault,0,a[2]};
                at=static_cast<std::size_t>(end-bytes.begin())+1;
            }
        }
        if (a[3]) {
            const auto out=ram_offset(a[3],4,ram.size());
            if (!out || ram.size()-*out<4 || 0x200000-*out<4)
                return {NativeServiceStatus::address_fault,0,a[3]};
            if (nt_iop_native_code_word(*out)) return {NativeServiceStatus::code_write,0,a[3]};
        }
        load_path_=path;
        load_bytes_=0;
        load_hash_=0;
        const auto device=device_class(path);
        const bool cdrom=device=="cdrom";
        const bool rom=device=="rom" && legal_boot_device(path);
        if (!cdrom && !rom) {
            load_decision_=4;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-201)};
        }
        // rom images are not read here yet. An unmounted device and a missing
        // cdrom file are both "no file", not a successful load.
        if (!cdrom || !files_) {
            load_decision_=5;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-203)};
        }
        Bytes image;
        try { image=files_->read_file(path,4ull*1024*1024); }
        catch (const disc::PathError&) {
            load_decision_=3;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-203)};
        } catch (const std::runtime_error&) {
            load_decision_=6;
            return {NativeServiceStatus::unavailable};
        }
        if (image.empty()) {
            load_decision_=6;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-204)};
        }
        load_bytes_=image.size();
        load_hash_=fingerprint(image);
        const auto sealed=std::find_if(sealed_.begin(),sealed_.end(),[&](const SealedModule& module) {
            return module.source_hash==load_hash_;
        });
        // A hash without a sealed entry is not a module start. The file stays
        // where conversion installed it; this path does not relocate it.
        if (sealed==sealed_.end()) {
            load_decision_=std::find(catalog_.begin(),catalog_.end(),load_hash_)==catalog_.end() ? 1 : 2;
            return {NativeServiceStatus::unavailable};
        }
        return begin_sealed(request,*sealed,path,a,ram);
    }
    // StartModule addresses a module id. This owner has no loaded-but-unstarted
    // image, so a known id is already started and every other id is unknown.
    // The name, argument span, and stack result pointer are not read.
    NativeServiceReply start_module(std::uint32_t id) {
        const auto known=id && ((pending_ && pending_->id==id) ||
            std::any_of(resident_.begin(),resident_.end(),[&](const ResidentModule& item) { return item.id==id; }));
        load_decision_=known ? 14 : 15;
        return {NativeServiceStatus::returned,static_cast<std::uint32_t>(known ? -206 : -202)};
    }
    NativeServiceReply finish_load(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!pending_ || request.context!=pending_->owner || !request.context || request.context->native_call_owner!=this)
            return {};
        const auto entry_result=request.arguments[0];
        if (pending_->result) {
            const auto stored=store_word(pending_->result,entry_result,ram);
            if (stored.status!=NativeServiceStatus::returned) return stored;
        }
        const bool unlink=(entry_result&3U)==1U;
        if (!unlink && pending_->info)
            store_word(pending_->info+8,(entry_result&3U)<<16,ram);
        release(pending_->stack,ram);
        if (unlink) {
            release(pending_->image,ram);
            release(pending_->arguments,ram);
        } else resident_.push_back({pending_->hash,pending_->id});
        auto caller=pending_->caller;
        caller.registers[2]=pending_->id;
        pending_.reset();
        load_decision_=8;
        NativeServiceReply reply{NativeServiceStatus::restored};
        reply.restore=caller;
        return reply;
    }
    // Reserve the installed image, build argc/argv plus a module record, and
    // transfer to the compiled entry. The caller's v0 becomes the module id
    // only after that entry returns. Entry result 1 releases the reservation;
    // every other low two-bit value leaves it resident.
    NativeServiceReply begin_sealed(const NativeServiceRequest& request,const SealedModule& module,
                                    const std::string& path,const std::array<std::uint32_t,4>& a,std::span<std::uint8_t> ram) {
        if (pending_) {
            load_decision_=12;
            return {NativeServiceStatus::unavailable};
        }
        if (!request.context || !heap_) {
            load_decision_=11;
            return {NativeServiceStatus::unavailable};
        }
        if (!module.memory_size || module.initialized_size>module.memory_size ||
            module.entry<module.base || module.entry-module.base>=module.memory_size) {
            load_decision_=9;
            return {NativeServiceStatus::unavailable};
        }
        if (module.initialized_size) {
            const auto image=ram_offset(module.base,1,ram.size());
            if (!image || ram.size()-*image<module.initialized_size || 0x200000-*image<module.initialized_size) {
                load_decision_=9;
                return {NativeServiceStatus::unavailable};
            }
            if (fingerprint(ram.subspan(*image,module.initialized_size))!=module.image_hash) {
                load_decision_=9;
                return {NativeServiceStatus::unavailable};
            }
        }
        const auto entry=ram_offset(module.entry,4,ram.size());
        if (!entry || !nt_iop_native_instruction(*entry)) {
            load_decision_=9;
            return {NativeServiceStatus::unavailable};
        }
        if (std::find_if(resident_.begin(),resident_.end(),[&](const ResidentModule& item) {
                return item.hash==module.source_hash; })!=resident_.end()) {
            load_decision_=10;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-206)};
        }
        std::vector<nt::ByteView> segments;
        segments.push_back(nt::ByteView(reinterpret_cast<const std::uint8_t*>(path.data()),path.size()));
        if (a[1]) {
            const auto args=*ram_offset(a[2],1,ram.size());
            const auto bytes=ram.subspan(args,a[1]);
            for (std::size_t at=0;at<bytes.size();) {
                const auto end=std::find(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.end(),0);
                segments.push_back(bytes.subspan(at,static_cast<std::size_t>(end-bytes.begin())-at));
                at=static_cast<std::size_t>(end-bytes.begin())+1;
            }
        }
        const auto argc=static_cast<std::uint32_t>(segments.size());
        std::uint32_t bytes=48U+(argc+1U)*4U;
        for (const auto& segment:segments) bytes+=static_cast<std::uint32_t>(segment.size())+1U;
        const auto reserved=heap_->invoke({4,{2,module.memory_size,module.base},NativeServiceKind::sysmem},ram);
        if (reserved.status!=NativeServiceStatus::returned) return reserved;
        if (reserved.value!=module.base) {
            if (reserved.value) release(reserved.value,ram);
            const auto occupied=heap_->invoke({9,{module.base},NativeServiceKind::sysmem},ram);
            const bool free=occupied.status==NativeServiceStatus::returned && (occupied.value&0x80000000U);
            load_decision_=13;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(free ? -400 : -205)};
        }
        const auto arguments=heap_->invoke({4,{0,bytes,0},NativeServiceKind::sysmem},ram);
        if (arguments.status!=NativeServiceStatus::returned || !arguments.value) {
            release(module.base,ram);
            if (arguments.status!=NativeServiceStatus::returned) return arguments;
            load_decision_=13;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-400)};
        }
        constexpr std::uint32_t stack_size=0x1000;
        const auto stack=heap_->invoke({4,{0,stack_size,0},NativeServiceKind::sysmem},ram);
        if (stack.status!=NativeServiceStatus::returned || !stack.value) {
            release(arguments.value,ram); release(module.base,ram);
            if (stack.status!=NativeServiceStatus::returned) return stack;
            load_decision_=13;
            return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-400)};
        }
        const auto rollback=[&] {
            release(stack.value,ram); release(arguments.value,ram); release(module.base,ram);
        };
        if (!bytes_writable(arguments.value,bytes,ram) || !bytes_writable(stack.value,stack_size,ram)) {
            rollback();
            return {NativeServiceStatus::code_write,0,arguments.value};
        }
        std::fill_n(ram.begin()+arguments.value,bytes,std::uint8_t{0});
        std::fill_n(ram.begin()+stack.value,stack_size,std::uint8_t{0});
        const auto info=arguments.value;
        std::uint32_t cursor=info+48U+(argc+1U)*4U;
        std::vector<std::uint32_t> argv;
        argv.reserve(argc);
        for (const auto& segment:segments) {
            std::copy(segment.begin(),segment.end(),ram.begin()+cursor);
            argv.push_back(cursor);
            cursor+=static_cast<std::uint32_t>(segment.size())+1U;
        }
        store_word(info+4,argv.front(),ram);
        store_word(info+8,2U<<16,ram);
        store_word(info+12,next_id_,ram);
        store_word(info+16,module.entry,ram);
        store_word(info+20,module.global_pointer,ram);
        store_word(info+24,module.base,ram);
        store_word(info+28,module.initialized_size,ram);
        store_word(info+36,module.memory_size-module.initialized_size,ram);
        for (std::uint32_t index=0;index<argc;++index) store_word(info+48U+index*4U,argv[index],ram);
        auto caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
        pending_=Pending{request.context,caller,a[3],next_id_,module.base,arguments.value,info,stack.value,module.source_hash};
        ++next_id_;
        load_decision_=7;
        NativeServiceReply reply{NativeServiceStatus::called,module.entry};
        NativeServiceCall call;
        call.arguments={argc,info+48U,0,info};
        call.global_pointer=module.global_pointer;
        call.owner=this;
        call.stack_pointer=stack.value+stack_size-16U;
        reply.call=call;
        return reply;
    }
    static bool bytes_writable(std::uint32_t address,std::uint32_t size,std::span<const std::uint8_t> ram) {
        if (!size) return true;
        const auto at=ram_offset(address,1,ram.size());
        if (!at || ram.size()-*at<size || 0x200000-*at<size) return false;
        for (std::uint32_t index=0;index<size;++index)
            if (nt_iop_native_code_word((*at+index)&~3U)) return false;
        return true;
    }
    static NativeServiceReply store_word(std::uint32_t address,std::uint32_t value,std::span<std::uint8_t> ram) {
        const auto at=ram_offset(address,4,ram.size());
        if (!at || ram.size()-*at<4 || 0x200000-*at<4) return {NativeServiceStatus::address_fault,0,address};
        if (nt_iop_native_code_word(*at)) return {NativeServiceStatus::code_write,0,address};
        store_native_word(ram,*at,value);
        return {NativeServiceStatus::returned};
    }
    void release(std::uint32_t address,std::span<std::uint8_t> ram) {
        if (address && heap_) heap_->invoke({5,{address,0,0,0},NativeServiceKind::sysmem},ram);
    }
    struct Pending {
        const Cpu* owner;
        Cpu caller;
        std::uint32_t result,id,image,arguments,info,stack;
        std::uint64_t hash;
    };
    NativeHeapServices* heap_=nullptr;
    std::vector<SealedModule> sealed_;
    struct ResidentModule { std::uint64_t hash=0; std::uint32_t id=0; };
    std::vector<ResidentModule> resident_;
    std::optional<Pending> pending_;
    std::uint32_t next_id_=1;
    std::uint32_t card_=0,disc_=0,path_=0,callbacks_=0;
    std::uint32_t boot_queries_=0;
    int boot_decision_=-1;
    std::string boot_device_;
    std::string boot_path_;
    std::unique_ptr<disc::Files> files_;
    std::vector<std::uint64_t> catalog_;
    int load_decision_=-1;
    std::uint64_t load_bytes_=0,load_hash_=0;
    std::string load_path_;
};
}
