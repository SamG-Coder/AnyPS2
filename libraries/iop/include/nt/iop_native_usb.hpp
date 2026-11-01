#pragma once
#include <nt/usb.hpp>
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_threads.hpp>

namespace nt::iop {
// Public USB driver registration is a native foreground continuation. Driver
// probe/connect callbacks execute compiled code with the registering module's GP.
// USB controller instructions and register banks are not part of this service.
class NativeUsbServices final : public NativeServices,public NativeCallbackSource {
public:
    NativeUsbServices(NativeLibraryServices& libraries,NativeHeapServices& heap,usb::Host& host)
        : libraries_(libraries),heap_(heap),host_(host) {}
    void connect(NativeThreadServices& threads) { threads_=&threads; }
    std::size_t device_count() const { return devices_.size(); }
    std::size_t driver_count() const { return drivers_.size(); }
    std::uint32_t error() const { return error_ ? error_ : host_.error(); }
    std::optional<NativeCallback> take() override {
        if (failure_) return {};
        for (auto& [id,pipe]:pipes_) {
            if (!pipe.pending) continue;
            std::vector<std::uint8_t> bytes;
            const auto state=pipe.input->poll(bytes);
            if (state==usb::TransferState::pending) continue;
            error_=state==usb::TransferState::failed ? pipe.input->error() : 0;
            auto pending=*pipe.pending; pipe.pending.reset();
            if (bytes.size()>pending.size) { fail(pending,NativeStop::address_fault); return {}; }
            const auto code=state==usb::TransferState::complete ? 0U : transfer_error(pipe.input->error());
            if (!code) std::copy(bytes.begin(),bytes.end(),arena_.begin()+pending.destination);
            if (!complete(pending,code,code ? 0U : static_cast<std::uint32_t>(bytes.size()))) return {};
        }
        return {};
    }
    void finish(std::uint32_t) override {} // Input completions run as foreground threads.
    std::optional<NativeResult> failure() const override { return failure_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) return finish(request);
        if (request.kind!=NativeServiceKind::usbd) return {};
        if (request.interrupt_context) return {NativeServiceStatus::unavailable};
        if (request.selector==0x100) return initialize(request,ram);
        if (!registered_) return {NativeServiceStatus::unavailable};
        if (request.selector==1) return returned(0);
        if (request.selector==9 || request.selector==12) {
            const auto id=request.arguments[0];
            if (!id || id>devices_.size() || pipes_.size()>=1024 || next_pipe_>0x7fffffff) return returned(0xffffffff);
            Pipe pipe; pipe.aligned=request.selector==12;
            if (request.arguments[1]) {
                const auto descriptor=ram_offset(request.arguments[1],1,ram.size());
                const auto& device=devices_[id-1]; usb::Endpoint endpoint;
                if (!descriptor || *descriptor<device.address ||
                    !usb::describe_endpoint(device.source,*descriptor-device.address,endpoint)) return returned(0xffffffff);
                if (!std::equal(device.source.descriptors.begin(),device.source.descriptors.end(),ram.begin()+device.address))
                    return {NativeServiceStatus::unavailable};
                pipe.input=host_.open_input(device.source,endpoint);
            } else pipe.control=host_.open_control(devices_[id-1].source);
            if (!pipe.input && !pipe.control) {
                error_=host_.error();
                // Actual permission, sharing, disconnect and resource failures
                // have a defined failed-open result. Missing implementations do not.
                if (error_==5 || error_==32 || error_==1167 || error_==8 || error_==14) return returned(0xffffffff);
                return {NativeServiceStatus::unavailable};
            }
            pipes_.emplace(next_pipe_,std::move(pipe));
            return returned(next_pipe_++);
        }
        if (request.selector==10) {
            const auto found=pipes_.find(request.arguments[0]);
            if (found==pipes_.end()) return returned(0x102);
            if (found->second.pending) {
                found->second.input->cancel();
                if (!complete(*found->second.pending,0x122,0)) return {NativeServiceStatus::unavailable};
            }
            pipes_.erase(found); return returned(0);
        }
        if (request.selector==13) {
            const auto id=request.arguments[0],address=request.arguments[1];
            if (!id || id>devices_.size()) return returned(0x101);
            const auto offset=ram_offset(address,1,ram.size());
            if (!offset || 7>ram.size()-*offset) return fault(address);
            for (auto word=*offset&~3U;word<*offset+7;word+=4)
                if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,address};
            std::array<std::uint8_t,7> path{};
            if (!host_.location(devices_[id-1].source,path)) {
                if (host_.error()==111) return returned(0x107);
                return {NativeServiceStatus::unavailable};
            }
            std::copy(path.begin(),path.end(),ram.begin()+*offset); return returned(0);
        }
        if (request.selector==11) return transfer(request,ram);
        if (request.selector==4 || request.selector==14) return register_driver(request,ram);
        if (request.selector==5 || request.selector==15) {
            if (pending_) return {NativeServiceStatus::unavailable};
            const auto address=ram_offset(request.arguments[0],4,ram.size());
            if (!address) return returned(0x104);
            const auto driver=find_driver(*address);
            if (driver==drivers_.end() || driver->autoloader!=(request.selector==15)) return returned(0x104);
            for (auto& device:devices_) if (device.driver==driver->address) { device.driver=0; device.private_data=0; }
            drivers_.erase(driver); return returned(0);
        }
        if (request.selector>=6 && request.selector<=8) {
            const auto id=request.arguments[0];
            if (!id || id>devices_.size()) return returned(request.selector==7 ? 0x101 : 0);
            auto& device=devices_[id-1];
            if (request.selector==7) { device.private_data=request.arguments[1]; return returned(0); }
            if (request.selector==8) return returned(device.private_data);
            // The prior pointer must identify an actual descriptor boundary.
            std::size_t next=0;
            if (request.arguments[1]) {
                const auto prior=ram_offset(request.arguments[1],1,ram.size());
                if (!prior || *prior<device.address || *prior-device.address>=device.bytes) return returned(0);
                bool found=false;
                for (std::size_t at=0;at<device.bytes;at+=device.source.descriptors[at])
                    if (device.address+at==*prior) { next=at+device.source.descriptors[at]; found=true; break; }
                if (!found) return returned(0);
            }
            if (device.address>ram.size() || device.bytes>ram.size()-device.address) return fault(device.address);
            if (!std::equal(device.source.descriptors.begin(),device.source.descriptors.end(),ram.begin()+device.address))
                return {NativeServiceStatus::unavailable};
            for (auto at=next;at<device.bytes;at+=device.source.descriptors[at])
                if (!request.arguments[2] || device.source.descriptors[at+1]==request.arguments[2])
                    return returned(device.address+static_cast<std::uint32_t>(at));
            return returned(0);
        }
        // Unsupported device classes and hotplug still require native providers.
        // Never fabricate their completion.
        return {NativeServiceStatus::unavailable};
    }
private:
    struct Device { usb::Device source; std::uint32_t address,bytes,private_data=0,driver=0; };
    struct Driver { std::uint32_t address,probe,connect,disconnect,gp; bool autoloader; };
    struct Pending { const Cpu* owner; Cpu caller; std::uint32_t driver; std::size_t device=0; bool connecting=false; };
    struct Transfer { std::uint32_t destination,size,callback,argument,gp,pc; };
    struct Pipe {
        std::unique_ptr<usb::ControlPipe> control;
        std::unique_ptr<usb::InputPipe> input;
        std::optional<Transfer> pending;
        bool aligned=false;
    };
    static std::uint32_t transfer_error(std::uint32_t error) {
        if (error==995) return 0x122; // CancelIoEx completion.
        if (error==1167 || error==121) return 5; // Disconnected or timed out.
        if (error==122) return 8; // More data than the caller's buffer.
        return 0xe; // Host transfer was not accessed successfully.
    }
    void fail(const Transfer& transfer,NativeStop reason) { failure_=NativeResult{reason,transfer.pc,transfer.destination}; }
    bool complete(const Transfer& transfer,std::uint32_t status,std::uint32_t size) {
        if (!transfer.callback) return true;
        const auto result=threads_->post_callback(this,transfer.callback,{status,size,transfer.argument,0},transfer.gp,arena_);
        if (result.status==NativeServiceStatus::returned) return true;
        fail(transfer,NativeStop::service_request); return false;
    }
    static NativeServiceReply returned(std::uint32_t value) { return {NativeServiceStatus::returned,value}; }
    static NativeServiceReply fault(std::uint32_t address) { return {NativeServiceStatus::address_fault,0,address}; }
    std::vector<Driver>::iterator find_driver(std::uint32_t address) { return std::find_if(drivers_.begin(),drivers_.end(),[&](const auto& d) { return d.address==address; }); }
    NativeServiceReply transfer(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        const auto found=pipes_.find(a[0]);
        if (found==pipes_.end()) return returned(0x102);
        if (a[2]>65535) return returned(0x103);
        if (found->second.aligned && (a[1]&3)) return returned(0x106);
        if (found->second.pending) return returned(0x121);
        const bool input=found->second.input!=nullptr;
        if (input && a[3]) return returned(0x113);
        const auto setup=ram_offset(a[3],1,ram.size());
        if (!input && (!setup || 8>ram.size()-*setup)) return fault(a[3]);
        const auto word=[&](unsigned offset) { return std::uint16_t(ram[*setup+offset]|(unsigned(ram[*setup+offset+1])<<8)); };
        const bool descriptor=!input && ram[*setup]==0x80 && ram[*setup+1]==6 && (word(2)>>8)>=1 && (word(2)>>8)<=5;
        const bool configure=!input && ram[*setup]==0 && ram[*setup+1]==9 && word(2)<=255 && !word(4) && !a[2];
        const bool select_interface=!input && ram[*setup]==1 && ram[*setup+1]==11 && word(2)<=255 && word(4)<=255 && !a[2];
        const bool report=!input && ram[*setup]==0x81 && ram[*setup+1]==6 && word(2)==0x2200 && word(4)<=255;
        const bool set_report=!input && ram[*setup]==0x21 && ram[*setup+1]==9 &&
            ((word(2)>>8)==2 || (word(2)>>8)==3) && word(4)<=255;
        if (!input && !descriptor && !configure && !select_interface && !report && !set_report)
            return {NativeServiceStatus::unavailable};
        if (!input && word(6)!=a[2]) return returned(0x103);
        const auto destination=ram_offset(a[1],1,ram.size());
        if (!destination || a[2]>ram.size()-*destination) return fault(a[1]);
        for (auto at=*destination&~3U;!set_report && at<*destination+a[2];at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,a[1]};
        if (request.stack_pointer>0xffffffe7U) return fault(request.stack_pointer);
        const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
        if (!stack || 8>ram.size()-*stack) return fault(request.stack_pointer);
        const auto callback=load_native_word(ram,*stack),argument=load_native_word(ram,*stack+4);
        if (callback) {
            const auto target=ram_offset(callback,4,ram.size());
            if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,callback};
            if (!threads_) return {NativeServiceStatus::unavailable};
        }
        if (input) {
            if (!found->second.input->submit(a[2])) return returned(transfer_error(found->second.input->error()));
            found->second.pending=Transfer{*destination,a[2],callback,argument,request.global_pointer,request.call_site};
            return returned(0);
        }
        std::vector<std::uint8_t> bytes;
        const bool completed=configure ? found->second.control->use_configuration(static_cast<std::uint8_t>(word(2))) :
            select_interface ? found->second.control->use_interface(static_cast<std::uint8_t>(word(4)),static_cast<std::uint8_t>(word(2))) :
            report ? found->second.control->read_report(static_cast<std::uint8_t>(word(4)),bytes) :
            set_report ? found->second.control->set_report(static_cast<std::uint8_t>(word(4)),static_cast<std::uint8_t>(word(2)>>8),
                static_cast<std::uint8_t>(word(2)),ram.subspan(*destination,a[2])) :
            found->second.control->read_descriptor(word(2),word(4),word(6),bytes);
        if (!completed) {
            error_=found->second.control->error();
            if (set_report && (error_==5 || error_==32 || error_==1167)) {
                const Transfer transfer{*destination,a[2],callback,argument,request.global_pointer,request.call_site};
                return complete(transfer,transfer_error(error_),0) ? returned(0) : NativeServiceReply{NativeServiceStatus::unavailable};
            }
            return {NativeServiceStatus::unavailable};
        }
        if (report && bytes.size()>a[2]) bytes.resize(a[2]);
        if (bytes.size()>a[2]) return {NativeServiceStatus::unavailable};
        if (callback) {
            const auto posted=threads_->post_callback(this,callback,{0,set_report ? a[2] : static_cast<std::uint32_t>(bytes.size()),argument,0},request.global_pointer,ram);
            if (posted.status!=NativeServiceStatus::returned) return posted;
        }
        std::copy(bytes.begin(),bytes.end(),ram.begin()+*destination); error_=0;
        return returned(0);
    }
    NativeServiceReply initialize(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (registered_) return {NativeServiceStatus::unavailable};
        const auto declaration=libraries_.declaration_for_entry("usbd",1,request.call_site);
        if (!declaration) return {NativeServiceStatus::unavailable};
        std::vector<usb::Device> snapshot;
        if (!host_.scan(snapshot)) return {NativeServiceStatus::unavailable};
        if (snapshot.size()>1024) return {NativeServiceStatus::unavailable};
        std::uint32_t bytes=0;
        for (const auto& device:snapshot) {
            if (!usb::valid_descriptors(device.descriptors) || device.descriptors.size()>0x100000-bytes)
                return {NativeServiceStatus::unavailable};
            // Each native device declaration has its own aligned base. Keep
            // the wire encoding within a device intact, including odd lengths.
            const auto allocation=(static_cast<std::uint32_t>(device.descriptors.size())+3U)&~3U;
            if (allocation>0x100000-bytes) return {NativeServiceStatus::unavailable};
            bytes+=allocation;
        }
        std::uint32_t address=0;
        if (bytes) {
            const auto allocated=heap_.invoke({4,{0,bytes},NativeServiceKind::sysmem},ram);
            if (allocated.status!=NativeServiceStatus::returned || !allocated.value) return {NativeServiceStatus::unavailable};
            address=allocated.value;
            if (address>ram.size() || bytes>ram.size()-address) { release(address,ram); return fault(address); }
            for (auto word=address&~3U;word<address+bytes;word+=4)
                if (nt_iop_native_code_word(word)) { release(address,ram); return {NativeServiceStatus::code_write,0,word}; }
        }
        std::vector<Device> devices; devices.reserve(snapshot.size());
        auto cursor=address;
        for (auto& source:snapshot) {
            const auto size=static_cast<std::uint32_t>(source.descriptors.size());
            devices.push_back({std::move(source),cursor,size}); cursor+=(size+3U)&~3U;
        }
        const auto registered=libraries_.invoke({6,{*declaration},NativeServiceKind::loadcore},ram);
        if (registered.status!=NativeServiceStatus::returned || registered.value) { release(address,ram); return {NativeServiceStatus::unavailable}; }
        for (const auto& device:devices) std::copy(device.source.descriptors.begin(),device.source.descriptors.end(),ram.begin()+device.address);
        devices_=std::move(devices); registered_=true; arena_=ram;
        return returned(0);
    }
    void release(std::uint32_t address,std::span<std::uint8_t> ram) {
        if (address) heap_.invoke({5,{address},NativeServiceKind::sysmem},ram);
    }
    NativeServiceReply register_driver(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (pending_ || !request.context) return {NativeServiceStatus::unavailable};
        const auto address=ram_offset(request.arguments[0],4,ram.size());
        if (!address || ram.size()-*address<48) return fault(request.arguments[0]);
        if (find_driver(*address)!=drivers_.end() || (request.selector==14 && std::any_of(drivers_.begin(),drivers_.end(),[](const auto& d) { return d.autoloader; })))
            return returned(0x121);
        if (load_native_word(ram,*address) || load_native_word(ram,*address+4) ||
            load_native_word(ram,*address+24) || load_native_word(ram,*address+28)) return returned(0x104);
        const auto name=ram_offset(load_native_word(ram,*address+8),1,ram.size());
        if (!name || !ram[*name]) return returned(0x104);
        const auto name_bytes=std::min<std::size_t>(256,ram.size()-*name);
        if (std::find(ram.begin()+*name,ram.begin()+*name+name_bytes,0)==ram.begin()+*name+name_bytes) return returned(0x104);
        Driver driver{*address,load_native_word(ram,*address+12),load_native_word(ram,*address+16),
                      load_native_word(ram,*address+20),request.global_pointer,request.selector==14};
        for (const auto callback:{driver.probe,driver.connect,driver.disconnect}) {
            const auto target=ram_offset(callback,4,0x200000);
            if (callback && (!target || !nt_iop_native_instruction(*target))) return {NativeServiceStatus::unmapped_code,0,callback};
        }
        // Until hotplug callbacks are implemented, fail explicitly if topology
        // changed. Never report a stale snapshot as a new successful discovery.
        std::vector<usb::Device> current;
        if (!host_.scan(current) || current.size()!=devices_.size()) return {NativeServiceStatus::unavailable};
        for (std::size_t i=0;i<current.size();++i) if (current[i]!=devices_[i].source) return {NativeServiceStatus::unavailable};
        auto caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
        drivers_.push_back(driver);
        pending_=Pending{request.context,caller,driver.address};
        auto next=next_device();
        return next.status==NativeServiceStatus::restored ? returned(0) : next;
    }
    NativeServiceReply call(std::uint32_t target,const Driver& driver) {
        NativeServiceReply reply{NativeServiceStatus::called,target};
        reply.call=NativeServiceCall{{static_cast<std::uint32_t>(pending_->device+1),0,0,0},driver.gp,this};
        return reply;
    }
    NativeServiceReply next_device() {
        const auto& driver=*find_driver(pending_->driver);
        while (pending_->device<devices_.size()) {
            auto& device=devices_[pending_->device];
            if (!device.driver && driver.probe) { device.private_data=0; return call(driver.probe,driver); }
            ++pending_->device;
        }
        auto caller=pending_->caller; caller.registers[2]=0;
        pending_.reset();
        NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
    }
    NativeServiceReply finish(const NativeServiceRequest& request) {
        if (!pending_ || !request.context || request.context!=pending_->owner || request.context->native_call_owner!=this) return {};
        const auto& driver=*find_driver(pending_->driver);
        if (!pending_->connecting && request.arguments[0]) {
            devices_[pending_->device].driver=driver.address;
            if (driver.connect) { pending_->connecting=true; return call(driver.connect,driver); }
        }
        ++pending_->device; pending_->connecting=false;
        return next_device();
    }
    NativeLibraryServices& libraries_;
    NativeHeapServices& heap_;
    usb::Host& host_;
    bool registered_=false;
    std::vector<Device> devices_;
    std::vector<Driver> drivers_;
    std::optional<Pending> pending_;
    NativeThreadServices* threads_=nullptr;
    std::map<std::uint32_t,Pipe> pipes_;
    std::uint32_t next_pipe_=1,error_=0;
    std::span<std::uint8_t> arena_;
    std::optional<NativeResult> failure_;
};
}
