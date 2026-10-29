#pragma once
#include <nt/iop_native_drivers.hpp>
#include <nt/iop_native_file_descriptors.hpp>
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_file_metadata.hpp>
#include <map>

namespace nt::iop {
// Compiled device operations share the mount descriptor namespace. Only public
// iop_file fields live in guest RAM; ownership and continuations stay native.
class NativeDriverFiles final : public NativeServices {
public:
    NativeDriverFiles(NativeDriverServices& drivers,NativeFileDescriptors& descriptors,NativeHeapServices& heap)
        : drivers_(drivers),descriptors_(descriptors),heap_(heap) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        auto reply=dispatch(request,ram);
        if (request.kind==NativeServiceKind::ioman && request.selector==30 &&
            reply.status==NativeServiceStatus::returned && !reply.high_return)
            reply.high_return=(reply.value&0x80000000U) ? 0xffffffffU : 0U;
        return reply;
    }
private:
    NativeServiceReply dispatch(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (request.kind==NativeServiceKind::native_return) return finish(request,ram);
        if (request.kind!=NativeServiceKind::ioman) return {};
        if (request.selector==4 || request.selector==13 || request.selector==25 || request.selector==32 || request.selector==26 || request.selector==27 || request.selector==28 || request.selector==29 || request.selector==31 || request.selector==33 || (request.selector>=10 && request.selector<=12))
            return path_call(request,ram);
        const bool fd_call=(request.selector>=5 && request.selector<=9) || request.selector==14 ||
            request.selector==15 || request.selector==22 || request.selector==30 || request.selector==34;
        if (!fd_call) {
            // Path operations for registered drivers must not fall into another mount.
            if ((request.selector>=10 && request.selector<=18) || (request.selector>=25 && request.selector<=33)) {
                Path path; const auto parsed=parse(request.arguments[0],ram,path);
                if (parsed.status!=NativeServiceStatus::returned || parsed.value) return parsed;
                if (drivers_.find(path.device)) return {NativeServiceStatus::unavailable};
            }
            return {};
        }
        const auto entry=descriptors_.find(request.arguments[0]);
        if (!entry) return request.arguments[0]>=3 ? returned(-9) : NativeServiceReply{};
        if (entry->owner!=this) return {};
        if (pending_ || request.interrupt_context || !request.context) return {NativeServiceStatus::unavailable};
        const auto found=files_.find(request.arguments[0]);
        if (found==files_.end()) return {NativeServiceStatus::unavailable};
        const auto& file=found->second;
        const bool directory_call=request.selector==14 || request.selector==15;
        const bool control=request.selector==9 || request.selector==22 || request.selector==34;
        if (!control && directory_call!=file.directory) return returned(file.directory && request.selector!=5 ? -21 : -9);
        const auto record=ram_offset(file.record,4,ram.size());
        if (!record || ram.size()-*record<16) return fault(file.record);
        const auto& device=drivers_.device(file.driver);
        if (load_native_word(ram,*record)!=file.mode || load_native_word(ram,*record+4)!=file.unit ||
            load_native_word(ram,*record+8)!=device.address) return {NativeServiceStatus::unavailable};
        if (request.selector==22) {
            const auto descriptor=ram_offset(device.address,4,ram.size());
            if (!descriptor || ram.size()-*descriptor<20 || 0x200000-*descriptor<20) return fault(device.address);
            if (load_native_word(ram,*descriptor+4)!=device.type) return {NativeServiceStatus::unavailable};
            return {NativeServiceStatus::returned,device.type};
        }
        if (request.selector==34) {
            if ((device.type&0xf0000000U)!=0x10000000U) return returned(-48);
            ControlBuffers buffers;
            const auto validated=control_buffers(request,ram,buffers);
            if (validated.status!=NativeServiceStatus::returned || validated.value) return validated;
            std::uint32_t target=0;
            const auto checked=operation(file.driver,26,ram,target);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            // O32 stack arguments retain the output pointer/length. The compiled
            // driver owns command semantics; the manager only validates extents.
            return begin(request,target,{file.record,request.arguments[1],request.arguments[2],request.arguments[3]},
                         file.driver,request.arguments[0],Action::value);
        }
        if (request.selector==6 || request.selector==7) {
            if (!(file.mode&(request.selector==6 ? 1U : 2U))) return returned(-9);
            const auto count=request.arguments[2];
            if (count>0x100000) return returned(-22);
            const auto checked=buffer(request.arguments[1],count,ram,request.selector==6);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        if (request.selector==8 && request.arguments[2]>2) return returned(-22);
        if (request.selector==30) {
            if ((device.type&0xf0000000U)!=0x10000000U) return returned(-48);
            if (request.stack_pointer>0xffffffffU-16) return fault(request.stack_pointer);
            const auto whence=ram_offset(request.stack_pointer+16,4,ram.size());
            if (!whence || ram.size()-*whence<4 || 0x200000-*whence<4) return fault(request.stack_pointer);
            if (load_native_word(ram,*whence)>2) return returned(-22);
            std::uint32_t target=0;
            const auto checked=operation(file.driver,22,ram,target);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            // Both public and device signatures use the same O32 layout: offset
            // in a2/a3, origin at sp+16. Preserve the caller's validated stack.
            return begin(request,target,{file.record,request.arguments[1],request.arguments[2],request.arguments[3]},
                         file.driver,request.arguments[0],Action::value64);
        }
        if (request.selector==15) {
            if (!ram_offset(request.arguments[1],4,ram.size())) return fault(request.arguments[1]);
            const auto checked=buffer(request.arguments[1],300,ram,true);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        std::uint32_t target=0;
        const auto checked=operation(file.driver,request.selector-1,ram,target);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (request.selector==15 && (device.type&0xf0000000U)==0x10000000U) {
            std::uint32_t scratch=0;
            const auto allocated=allocate_buffer(384,ram,scratch);
            if (allocated.status!=NativeServiceStatus::returned || allocated.value) return allocated;
            const auto output=*ram_offset(request.arguments[1],4,ram.size());
            if (scratch<output+300 && output<scratch+384) {
                release_memory(scratch,ram); return {NativeServiceStatus::unavailable};
            }
            std::fill_n(ram.begin()+scratch,384,0);
            auto reply=begin(request,target,{file.record,scratch,0,0},file.driver,request.arguments[0],Action::directory);
            pending_->scratch=scratch; pending_->output=request.arguments[1];
            return reply;
        }
        return begin(request,target,{file.record,request.arguments[1],request.arguments[2],0},
                     file.driver,request.arguments[0],request.selector==5 || request.selector==14 ? Action::close : Action::value);
    }
private:
    enum class Action { open, value, value64, close, directory };
    struct File { std::uint32_t record,mode,unit; std::size_t driver; bool directory; };
    struct Path { std::string device; std::uint32_t unit=0,relative=0,bytes=0; };
    struct Pending { const Cpu* owner; Cpu caller; std::uint32_t fd; Action action; std::uint32_t scratch=0,output=0; bool normalize_result=false; };
    static NativeServiceReply returned(std::int32_t value) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    static NativeServiceReply fault(std::uint32_t address) { return {NativeServiceStatus::address_fault,0,address}; }
    static NativeServiceReply path_text(std::uint32_t address,std::span<const std::uint8_t> ram,std::string& text) {
        const auto start=ram_offset(address,1,ram.size());
        if (!start) return fault(address);
        const auto count=std::min<std::size_t>({256,ram.size()-*start,0x200000-*start});
        const auto bytes=ram.subspan(*start,count);
        const auto end=std::find(bytes.begin(),bytes.end(),0);
        if (end==bytes.end()) return count==256 ? returned(-36) : fault(address);
        text.assign(bytes.begin(),end);
        return returned(0);
    }
    static NativeServiceReply parse(std::uint32_t address,std::span<const std::uint8_t> ram,Path& path) {
        std::string text;
        const auto checked=path_text(address,ram,text);
        if (checked.status!=NativeServiceStatus::returned || checked.value) return checked;
        const auto colon=text.find(':');
        if (colon==std::string::npos || !colon) return returned(-19);
        auto name_end=colon;
        while (name_end && text[name_end-1]>='0' && text[name_end-1]<='9') --name_end;
        if (!name_end) return returned(-19);
        for (auto at=name_end;at<colon;++at) {
            const auto digit=static_cast<unsigned>(text[at]-'0');
            if (path.unit>(0x7fffffffU-digit)/10) return returned(-22);
            path.unit=path.unit*10+digit;
        }
        path.device=text.substr(0,name_end);
        path.bytes=static_cast<std::uint32_t>(text.size()+1);
        path.relative=*ram_offset(address,1,ram.size())+static_cast<std::uint32_t>(colon)+1;
        return returned(0);
    }
    NativeServiceReply second_path(std::uint32_t address,const Path& original,std::size_t driver,
                                   std::span<const std::uint8_t> ram,std::uint32_t& relative) const {
        if (!address) return returned(-18);
        std::string text;
        const auto checked=path_text(address,ram,text);
        if (checked.status!=NativeServiceStatus::returned || checked.value) return checked;
        relative=address;
        if (text.find(':')==std::string::npos) return returned(0);
        Path path;
        const auto parsed=parse(address,ram,path);
        if (parsed.status!=NativeServiceStatus::returned) return parsed;
        if (parsed.value || drivers_.find(path.device)!=std::optional<std::size_t>{driver} || path.unit!=original.unit)
            return returned(-18);
        relative=path.relative;
        return returned(0);
    }
    static NativeServiceReply buffer(std::uint32_t address,std::uint32_t bytes,std::span<const std::uint8_t> ram,bool write) {
        if (!bytes) return returned(0);
        const auto start=ram_offset(address,1,ram.size());
        if (!start || bytes>ram.size()-*start || bytes>0x200000-*start) return fault(address);
        if (write) for (auto word=*start&~3U;word<*start+bytes;word+=4)
            if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,address};
        return returned(0);
    }
    NativeServiceReply operation(std::size_t driver,unsigned ordinal,std::span<const std::uint8_t> ram,std::uint32_t& target) const {
        const auto& device=drivers_.device(driver);
        const auto descriptor=ram_offset(device.address,4,ram.size());
        if (!descriptor || ram.size()-*descriptor<20) return fault(device.address);
        if (load_native_word(ram,*descriptor+16)!=device.operations || load_native_word(ram,*descriptor+4)!=device.type) return {NativeServiceStatus::unavailable};
        const auto ops=ram_offset(device.operations,4,ram.size());
        const auto size=(ordinal+1)*4;
        if (!ops || size>ram.size()-*ops || size>0x200000-*ops) return fault(device.operations);
        target=load_native_word(ram,*ops+ordinal*4);
        const auto code=ram_offset(target,4,0x200000);
        if (!target || !code || !nt_iop_native_instruction(*code)) return {NativeServiceStatus::unmapped_code,0,target};
        return returned(0);
    }
    struct ControlBuffers { std::uint32_t output=0,size=0; };
    static NativeServiceReply control_buffers(const NativeServiceRequest& request,std::span<const std::uint8_t> ram,ControlBuffers& buffers) {
        if (request.stack_pointer>0xffffffffU-23) return fault(request.stack_pointer);
        const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
        if (!stack || ram.size()-*stack<8 || 0x200000-*stack<8) return fault(request.stack_pointer);
        const auto output=load_native_word(ram,*stack),size=load_native_word(ram,*stack+4);
        if (request.arguments[3]>0x100000 || size>0x100000) return returned(-22);
        const auto input_check=buffer(request.arguments[2],request.arguments[3],ram,false);
        if (input_check.status!=NativeServiceStatus::returned) return input_check;
        const auto output_check=buffer(output,size,ram,true);
        if (output_check.status!=NativeServiceStatus::returned) return output_check;
        buffers={output,size};
        return returned(0);
    }
    NativeServiceReply path_call(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        Path path; const auto parsed=parse(request.arguments[0],ram,path);
        if (parsed.status!=NativeServiceStatus::returned || parsed.value) return parsed;
        const auto driver=drivers_.find(path.device);
        if (!driver) return {};
        if (pending_ || request.interrupt_context || !request.context) return {NativeServiceStatus::unavailable};
        std::uint32_t target=0;
        const bool directory=request.selector==13;
        const bool device_control=request.selector==31;
        const bool mount=request.selector==28;
        const bool readlink=request.selector==33;
        const bool two_paths=request.selector==25 || request.selector==32;
        const bool extended_path=request.selector==26 || request.selector==27 || request.selector==29 || readlink || two_paths || mount;
        const bool temporary=device_control || extended_path || (request.selector>=10 && request.selector<=12);
        const auto kind=drivers_.device(*driver).type&0xf0000000U;
        if (directory && kind && kind!=0x10000000U) return {NativeServiceStatus::unavailable};
        std::uint32_t destination=0;
        if (two_paths) {
            const auto checked=second_path(request.arguments[1],path,*driver,ram,destination);
            if (checked.status!=NativeServiceStatus::returned || checked.value) return checked;
        }
        if (extended_path && kind!=0x10000000U) return returned(-48);
        if (readlink) {
            if (request.arguments[2]>0x100000) return returned(-22);
            const auto checked=buffer(request.arguments[1],request.arguments[2],ram,true);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        ControlBuffers buffers;
        std::uint32_t frame=0,mount_size=0,device_name_size=0;
        if (device_control || mount) {
            if (kind!=0x10000000U) return returned(-48);
            if (device_control) {
                const auto checked=control_buffers(request,ram,buffers);
                if (checked.status!=NativeServiceStatus::returned || checked.value) return checked;
            } else {
                if (request.stack_pointer>0xffffffffU-19) return fault(request.stack_pointer);
                const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
                if (!stack || ram.size()-*stack<4 || 0x200000-*stack<4) return fault(request.stack_pointer);
                mount_size=load_native_word(ram,*stack);
                if (mount_size>0x100000) return returned(-22);
                const auto checked=buffer(request.arguments[3],mount_size,ram,false);
                if (checked.status!=NativeServiceStatus::returned) return checked;
                if (request.arguments[1]) {
                    std::string name;
                    const auto checked_name=path_text(request.arguments[1],ram,name);
                    if (checked_name.status!=NativeServiceStatus::returned || checked_name.value) return checked_name;
                    device_name_size=static_cast<std::uint32_t>(name.size()+1);
                }
            }
            if (request.stack_pointer<32 || request.stack_pointer%8) return fault(request.stack_pointer);
            const auto writable=buffer(request.stack_pointer-32,32,ram,true);
            if (writable.status!=NativeServiceStatus::returned) return writable;
            frame=*ram_offset(request.stack_pointer-32,4,ram.size());
            const auto overlaps=[&](std::uint32_t address,std::uint32_t bytes) {
                if (!bytes) return false;
                const auto start=ram_offset(address,1,ram.size());
                return start && *start<frame+32 && frame<*start+bytes;
            };
            // The outgoing argument frame must not overwrite a live payload or path.
            if (overlaps(request.arguments[0],path.bytes) ||
                (device_control && (overlaps(request.arguments[2],request.arguments[3]) || overlaps(buffers.output,buffers.size))) ||
                (mount && (overlaps(request.arguments[3],mount_size) || overlaps(request.arguments[1],device_name_size))))
                return {NativeServiceStatus::unavailable};
        }
        const auto mode=temporary ? 0U : directory ? (kind ? 8U : 0U) : request.arguments[1];
        const auto slot=device_control ? 23U : extended_path ? request.selector-8 : temporary ? request.selector-1 : directory ? 12U : 3U;
        const auto checked=operation(*driver,slot,ram,target);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        std::uint32_t record=0;
        const auto allocated=allocate_buffer(16,ram,record);
        if (allocated.status!=NativeServiceStatus::returned || allocated.value) return allocated;
        if ((device_control || mount) && record<frame+32 && frame<record+16) {
            release_memory(record,ram); return {NativeServiceStatus::unavailable};
        }
        const auto fd=descriptors_.allocate(this,record);
        if (!fd) { release_memory(record,ram); return returned(-24); }
        try { files_.emplace(*fd,File{record,mode,path.unit,*driver,directory}); }
        catch (...) { descriptors_.release(*fd,this); release_memory(record,ram); throw; }
        drivers_.retain(*driver);
        store_native_word(ram,record,mode); store_native_word(ram,record+4,path.unit);
        store_native_word(ram,record+8,drivers_.device(*driver).address); store_native_word(ram,record+12,0);
        if (mount) {
            store_native_word(ram,frame+16,request.arguments[3]); store_native_word(ram,frame+20,mount_size);
            auto reply=begin(request,target,{record,path.relative,request.arguments[1],request.arguments[2]},*driver,*fd,Action::close);
            reply.call->stack_pointer=request.stack_pointer-32;
            pending_->normalize_result=true;
            return reply;
        }
        if (device_control) {
            store_native_word(ram,frame+16,request.arguments[3]);
            store_native_word(ram,frame+20,buffers.output); store_native_word(ram,frame+24,buffers.size);
            auto reply=begin(request,target,{record,path.relative,request.arguments[1],request.arguments[2]},*driver,*fd,Action::close);
            reply.call->stack_pointer=request.stack_pointer-32;
            return reply;
        }
        const auto argument=two_paths ? destination : (request.selector==27 || readlink) ? request.arguments[1] : temporary ? (request.selector==11 && kind==0x10000000U ? 0755U : 0U) :
            directory ? 0U : request.arguments[1];
        auto reply=begin(request,target,{record,path.relative,argument,readlink ? request.arguments[2] : 0U},*driver,*fd,temporary ? Action::close : Action::open);
        pending_->normalize_result=extended_path && !readlink;
        return reply;
    }
    NativeServiceReply begin(const NativeServiceRequest& request,std::uint32_t target,std::array<std::uint32_t,4> args,
                             std::size_t driver,std::uint32_t fd,Action action) {
        Cpu caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
        pending_=Pending{request.context,caller,fd,action};
        NativeServiceReply reply{NativeServiceStatus::called,target};
        reply.call=NativeServiceCall{args,drivers_.device(driver).global_pointer,this}; return reply;
    }
    NativeServiceReply allocate_buffer(std::uint32_t bytes,std::span<std::uint8_t> ram,std::uint32_t& address) {
        const auto allocated=heap_.invoke({4,{0,bytes,0,0},NativeServiceKind::sysmem},ram);
        if (allocated.status!=NativeServiceStatus::returned) return allocated;
        if (!allocated.value) return returned(-12);
        const auto checked=buffer(allocated.value,bytes,ram,true);
        if (checked.status!=NativeServiceStatus::returned) { release_memory(allocated.value,ram); return checked; }
        address=allocated.value; return returned(0);
    }
    void release_memory(std::uint32_t record,std::span<std::uint8_t> ram) {
        heap_.invoke({5,{record,0,0,0},NativeServiceKind::sysmem},ram);
    }
    void erase(std::uint32_t fd,std::span<std::uint8_t> ram) {
        const auto file=files_.at(fd);
        drivers_.release(file.driver); descriptors_.release(fd,this);
        release_memory(file.record,ram); files_.erase(fd);
    }
    NativeServiceReply finish(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!request.context || request.context->native_call_owner!=this || !pending_ || pending_->owner!=request.context) return {};
        const auto result=request.arguments[0];
        const auto pending=*pending_;
        if (pending.action==Action::close || (pending.action==Action::open && (result&0x80000000U))) {
            const auto arena=heap_.invoke({6,{},NativeServiceKind::sysmem},ram);
            if (arena.status!=NativeServiceStatus::returned) return arena;
        }
        if (pending.action==Action::directory) {
            // Cleanup also requires the original heap arena. Keep ownership on
            // a truncated-memory retry, including EOF and error completions.
            const auto arena=heap_.invoke({6,{},NativeServiceKind::sysmem},ram);
            if (arena.status!=NativeServiceStatus::returned) return arena;
            // Recheck the destination on return because callbacks may yield.
            // Keep the continuation intact on a fault so retry never reruns dread.
            if (result && !(result&0x80000000U)) {
                const auto checked=buffer(pending.output,300,ram,true);
                if (checked.status!=NativeServiceStatus::returned) return checked;
                const auto source=ram_offset(pending.scratch,4,ram.size());
                if (!source || ram.size()-*source<384) return fault(pending.scratch);
                const auto converted=legacy_directory_record(ram.subspan(*source,384));
                const auto output=*ram_offset(pending.output,4,ram.size());
                std::copy(converted.begin(),converted.end(),ram.begin()+output);
            }
            release_memory(pending.scratch,ram);
        }
        auto caller=pending.caller; caller.registers[2]=pending.normalize_result && !(result&0x80000000U) ? 0U : result;
        if (pending.action==Action::value64) caller.registers[3]=request.arguments[1];
        if (pending.action==Action::open) {
            if (result&0x80000000U) erase(pending.fd,ram);
            else caller.registers[2]=pending.fd;
        } else if (pending.action==Action::close) erase(pending.fd,ram);
        pending_.reset();
        NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
    }
    NativeDriverServices& drivers_;
    NativeFileDescriptors& descriptors_;
    NativeHeapServices& heap_;
    std::map<std::uint32_t,File> files_;
    std::optional<Pending> pending_;
};
}
