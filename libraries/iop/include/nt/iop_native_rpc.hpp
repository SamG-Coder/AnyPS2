#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_sif_endpoint.hpp>
#include <algorithm>
#include <map>
#include <vector>

namespace nt::iop {
// Native ownership of RPC queues and server registrations. Public descriptor
// fields remain visible in RAM; no guest linked list is followed as host code.
class NativeRpcServers final : public NativeServices {
public:
    explicit NativeRpcServers(NativeSifCommandEndpoint* peer=nullptr,NativeServices* workers=nullptr) : peer_(peer),workers_(workers) {}
    // Build a stable reply before attempting delivery. The inbound owner retains
    // this snapshot across backpressure, so later registrations cannot change it.
    NativeServiceReply prepare_bind(std::span<const std::uint8_t> packet,std::span<const std::uint8_t> ram,
                                    std::array<std::uint8_t,64>& reply) const {
        if (packet.size()<36 || packet.size()>112 || load_native_word(packet,0)!=packet.size() ||
            load_native_word(packet,8)!=0x80000009) return {};
        std::uint32_t server=0,buffer=0;
        const auto sid=load_native_word(packet,32);
        for (const auto queue : order_) {
            for (const auto offset : queues_.at(queue).servers) {
                if (offset>ram.size() || ram.size()-offset<68) return {NativeServiceStatus::address_fault,0,offset};
                if (load_native_word(ram,offset)!=sid) continue;
                const auto function=load_native_word(ram,offset+4);
                const auto code=ram_offset(function,4,0x200000);
                if (!function || !code || !nt_iop_native_instruction(*code))
                    return {NativeServiceStatus::unmapped_code,0,function};
                server=servers_.at(offset).address; buffer=load_native_word(ram,offset+8); break;
            }
            if (server) break;
        }
        std::array<std::uint8_t,64> prepared{};
        store_native_word(prepared,0,64); store_native_word(prepared,8,0x80000008);
        store_native_word(prepared,20,load_native_word(packet,20));
        store_native_word(prepared,28,load_native_word(packet,28));
        store_native_word(prepared,32,0x80000009);
        store_native_word(prepared,36,server); store_native_word(prepared,40,buffer);
        reply=prepared;
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply send_bind_reply(std::span<const std::uint8_t> reply) {
        if (!peer_) return {};
        return peer_->submit(reply,{});
    }
    NativeServiceReply receive_call(std::span<const std::uint8_t> packet,std::span<const std::uint8_t> payload,
                                   std::span<std::uint8_t> ram) {
        if (packet.size()<56 || packet.size()>112 || packet[0]!=packet.size() ||
            (load_native_word(packet,0)>>8)!=payload.size() || payload.size()>0x100000 ||
            load_native_word(packet,8)!=0x8000000a || load_native_word(packet,36)!=payload.size() ||
            load_native_word(packet,44)>0x100000 || load_native_word(packet,48)>1) return {};
        const auto address=load_native_word(packet,52);
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset) return {NativeServiceStatus::address_fault,0,address};
        const auto found=servers_.find(*offset);
        if (found==servers_.end() || found->second.state!=RequestState::idle) return {};
        auto& server=found->second;
        auto& queue=queues_.at(server.queue);
        if (!queue.published) return {};
        for (const auto [at,size] : {std::pair{*offset,68U},std::pair{server.queue,24U}}) {
            const auto checked=writable(at,size,ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        const auto function=load_native_word(ram,*offset+4);
        const auto code=ram_offset(function,4,0x200000);
        if (!function || !code || !nt_iop_native_instruction(*code)) return {NativeServiceStatus::unmapped_code,0,function};
        std::uint32_t data=0;
        if (!payload.empty()) {
            const auto buffer=load_native_word(ram,*offset+8);
            const auto checked=writable(buffer,static_cast<std::uint32_t>(payload.size()),ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            data=*ram_offset(buffer,4,ram.size());
            const auto destination=ram_offset(load_native_word(packet,4),4,ram.size());
            if (!destination || *destination!=data || overlaps(data,static_cast<std::uint32_t>(payload.size()))) return {};
            for (const auto& [at,other] : servers_)
                if (other.state!=RequestState::idle && other.data_size && data<other.data+other.data_size && other.data<data+payload.size()) return {};
        }
        const auto link=queue.pending.empty() ? server.queue+12 : queue.pending.back()+60;
        const auto link_check=writable(link,4,ram);
        if (link_check.status!=NativeServiceStatus::returned) return link_check;
        // Prepare storage before waking a worker. All remaining writes are bounded
        // and nonthrowing, so it cannot observe a partially admitted request.
        std::vector<std::uint8_t> bytes(payload.begin(),payload.end());
        queue.pending.reserve(queue.pending.size()+1);
        const std::array<std::pair<unsigned,unsigned>,8> fields{{{12,36},{28,28},{32,20},{36,32},{40,40},{44,44},{48,48},{52,16}}};
        std::array<std::uint32_t,8> values{};
        for (unsigned index=0;index<fields.size();++index) values[index]=load_native_word(packet,fields[index].second);
        const auto thread=load_native_word(ram,server.queue);
        if (!(thread&0x80000000U) && !queue.active) {
            if (!workers_) return {};
            const auto wake=workers_->invoke({26,{thread,0,0,0},NativeServiceKind::thbase,0,true},ram);
            if (wake.status!=NativeServiceStatus::returned || wake.value) return wake;
        }
        if (!bytes.empty()) std::copy(bytes.begin(),bytes.end(),ram.begin()+data);
        for (unsigned index=0;index<fields.size();++index) store_native_word(ram,*offset+fields[index].first,values[index]);
        store_native_word(ram,*offset+60,0); store_native_word(ram,link,address); store_native_word(ram,server.queue+16,address);
        queue.pending.push_back(*offset); server.request_address=address; server.state=RequestState::queued;
        server.data=data; server.data_size=static_cast<std::uint32_t>(bytes.size());
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) return finish_request(request,ram);
        if (request.kind!=NativeServiceKind::sifcmd) return {};
        if (request.selector!=17 && request.selector!=19 && request.selector!=20 && request.selector!=21 && request.selector!=22 && request.selector!=24 && request.selector!=25) return {};
        if (request.interrupt_context) return {};
        const auto& a=request.arguments;
        if (request.selector==22) return run_loop(request,ram);
        if (request.selector==21) return execute_request(request,ram);
        if (request.selector==19) return create_queue(a[0],a[1],ram);
        if (request.selector==17) return register_server(request,ram);
        if (request.selector==20) {
            const auto checked=writable(a[0],24,ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            const auto offset=*ram_offset(a[0],4,ram.size());
            const auto found=queues_.find(offset);
            if (found==queues_.end()) return {};
            auto& queue=found->second;
            queue.active=!queue.pending.empty(); store_native_word(ram,offset+4,queue.active ? 1 : 0);
            if (!queue.active) { store_native_word(ram,offset+12,0); return {NativeServiceStatus::returned}; }
            auto& server=servers_.at(queue.pending.front());
            queue.pending.erase(queue.pending.begin()); server.state=RequestState::active;
            store_native_word(ram,offset+12,queue.pending.empty() ? 0 : servers_.at(queue.pending.front()).request_address);
            return {NativeServiceStatus::returned,server.request_address};
        }
        const auto queue_offset=ram_offset(request.selector==24 ? a[1] : a[0],4,ram.size());
        if (!queue_offset) return {NativeServiceStatus::address_fault,0,request.selector==24 ? a[1] : a[0]};
        const auto found=queues_.find(*queue_offset);
        if (found==queues_.end()) return {NativeServiceStatus::returned};
        auto& queue=found->second;
        if (request.selector==24) {
            const auto offset=ram_offset(a[0],4,ram.size());
            if (!offset) return {NativeServiceStatus::address_fault,0,a[0]};
            const auto item=std::find(queue.servers.begin(),queue.servers.end(),*offset);
            if (item==queue.servers.end()) return {NativeServiceStatus::returned};
            if (servers_.at(*offset).state!=RequestState::idle) return {};
            const auto index=static_cast<std::size_t>(item-queue.servers.begin());
            const auto link=index ? queue.servers[index-1]+56 : *queue_offset+8;
            const auto checked=writable(link,4,ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            const auto next=index+1<queue.servers.size() ? servers_.at(queue.servers[index+1]).address : 0;
            store_native_word(ram,link,next);
            queue.servers.erase(item); servers_.erase(*offset);
            if (!queue.published && queue.servers.empty()) queues_.erase(found);
            return {NativeServiceStatus::returned,a[0]};
        }
        for (const auto server : queue.servers) if (servers_.at(server).state!=RequestState::idle) return {};
        const auto item=std::find(order_.begin(),order_.end(),*queue_offset);
        if (item==order_.end()) return {NativeServiceStatus::returned};
        if (item!=order_.begin()) {
            const auto link=*(item-1)+20;
            const auto checked=writable(link,4,ram);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            const auto next=item+1!=order_.end() ? queues_.at(*(item+1)).address : 0;
            store_native_word(ram,link,next);
        }
        order_.erase(item); queue.published=false;
        if (queue.servers.empty()) queues_.erase(found);
        return {NativeServiceStatus::returned,a[0]};
    }
private:
    NativeServiceReply run_loop(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!request.context || !workers_ || !peer_ || !peer_->accepting()) return {};
        const auto checked=writable(request.arguments[0],24,ram);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto offset=*ram_offset(request.arguments[0],4,ram.size());
        const auto found=queues_.find(offset);
        if (found==queues_.end() || !found->second.published) return {};
        const auto worker=workers_->invoke({20,{},NativeServiceKind::thbase},ram);
        if (worker.status!=NativeServiceStatus::returned || worker.value!=load_native_word(ram,offset)) return {};
        auto& queue=found->second;
        if (queue.pending.empty()) {
            const auto slept=workers_->invoke({24,{},NativeServiceKind::thbase},ram);
            if (slept.status!=NativeServiceStatus::yielded &&
                (slept.status!=NativeServiceStatus::returned || slept.value)) return slept;
            queue.active=false; store_native_word(ram,offset+4,0); store_native_word(ram,offset+12,0);
            if (slept.status==NativeServiceStatus::yielded) return {NativeServiceStatus::retry_yielded};
            // A queued wakeup was consumed. Recheck through the ordinary budgeted
            // native service boundary instead of spinning inside the provider.
            return {NativeServiceStatus::transferred,request.call_site};
        }
        auto call=request;
        call.arguments[0]=servers_.at(queue.pending.front()).request_address;
        call.resume_pc=request.call_site;
        const auto result=execute_request(call,ram,true);
        if (result.status!=NativeServiceStatus::called) return result;
        // Handler validation and continuation allocation succeeded. Commit dequeue
        // only now, retaining FIFO ownership on unsupported or malformed requests.
        queue.pending.erase(queue.pending.begin()); queue.active=true;
        store_native_word(ram,offset+4,1);
        store_native_word(ram,offset+12,queue.pending.empty() ? 0 : servers_.at(queue.pending.front()).request_address);
        return result;
    }
    struct Invocation {
        Cpu caller;
        std::uint32_t server;
        bool prepared=false;
        bool notify=true;
        std::uint32_t record_address=0,data_address=0;
        std::array<std::uint8_t,64> reply{};
        std::vector<std::uint8_t> payload;
    };
    NativeServiceReply execute_request(const NativeServiceRequest& request,std::span<std::uint8_t> ram,bool queued=false) {
        if (!request.context || !peer_ || !peer_->accepting() || invocations_.contains(request.context) || invocations_.size()>=128) return {};
        const auto offset=ram_offset(request.arguments[0],4,ram.size());
        if (!offset || ram.size()-*offset<68 || 0x200000-*offset<68)
            return {NativeServiceStatus::address_fault,0,request.arguments[0]};
        const auto found=servers_.find(*offset);
        if (found==servers_.end() || found->second.state!=(queued ? RequestState::queued : RequestState::active) || load_native_word(ram,*offset+48)>1) return {};
        auto& server=found->second;
        const auto function=load_native_word(ram,*offset+4),buffer=load_native_word(ram,*offset+8),size=load_native_word(ram,*offset+12);
        const auto code=ram_offset(function,4,0x200000);
        if (!function || !code || !nt_iop_native_instruction(*code)) return {NativeServiceStatus::unmapped_code,0,function};
        const auto data=ram_offset(buffer,1,ram.size());
        if (size!=server.data_size || (size && (!data || *data!=server.data || size>ram.size()-*data))) return {};
        Cpu caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[2]=0; caller.registers[0]=0;
        NativeServiceReply result{NativeServiceStatus::called,function};
        result.call=NativeServiceCall{{load_native_word(ram,*offset+36),buffer,size,0},server.global_pointer};
        invocations_.emplace(request.context,Invocation{caller,*offset});
        server.state=RequestState::executing;
        return result;
    }
    NativeServiceReply finish_request(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (request.interrupt_context) return {};
        const auto found=invocations_.find(request.context);
        if (found==invocations_.end()) return {};
        auto& invocation=found->second;
        if (!invocation.prepared) {
            const auto offset=invocation.server;
            if (offset>ram.size() || ram.size()-offset<68) return {NativeServiceStatus::address_fault,0,offset};
            const auto mode=load_native_word(ram,offset+48);
            if (mode>1) return {};
            invocation.notify=mode!=0;
            invocation.record_address=load_native_word(ram,offset+32);
            invocation.data_address=load_native_word(ram,offset+40);
            const auto pointer=request.arguments[0];
            const auto count=pointer ? load_native_word(ram,offset+44) : 0;
            if (count>0x100000) return {};
            if (count) {
                const auto source=ram_offset(pointer,1,ram.size());
                if (!source || count>ram.size()-*source || count>0x200000-*source)
                    return {NativeServiceStatus::address_fault,0,pointer};
                invocation.payload.assign(ram.begin()+*source,ram.begin()+*source+count);
            }
            if (invocation.notify) {
                store_native_word(invocation.reply,0,64|(count<<8));
                store_native_word(invocation.reply,4,count ? invocation.data_address : 0);
                store_native_word(invocation.reply,8,0x80000008);
            }
            store_native_word(invocation.reply,28,load_native_word(ram,offset+28));
            store_native_word(invocation.reply,32,0x8000000a);
            invocation.prepared=true;
        }
        const auto sent=invocation.notify ? peer_->submit(invocation.reply,invocation.payload) :
            peer_->publish_result(invocation.record_address,invocation.reply,invocation.data_address,invocation.payload);
        if (sent.status!=NativeServiceStatus::returned) return sent;
        if (!sent.value) return {NativeServiceStatus::waiting};
        NativeServiceReply result{NativeServiceStatus::restored}; result.restore=invocation.caller;
        auto& server=servers_.at(invocation.server); server.state=RequestState::idle; server.data_size=0;
        invocations_.erase(found);
        return result;
    }
    enum class RequestState { idle,queued,active,executing };
    struct Queue {
        std::uint32_t address; bool published=true,active=false;
        std::vector<std::uint32_t> servers,pending;
    };
    struct Server {
        std::uint32_t address,queue,global_pointer;
        RequestState state=RequestState::idle;
        std::uint32_t request_address=0,data=0,data_size=0;
    };
    static NativeServiceReply writable(std::uint32_t address,std::uint32_t size,std::span<std::uint8_t> ram) {
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset)
            return {NativeServiceStatus::address_fault,0,address};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    bool overlaps(std::uint32_t address,std::uint32_t size) const {
        for (const auto& [at,queue] : queues_) if (address<at+24 && at<address+size) return true;
        for (const auto& [at,server] : servers_) if (address<at+68 && at<address+size) return true;
        return false;
    }
    NativeServiceReply create_queue(std::uint32_t address,std::uint32_t thread,std::span<std::uint8_t> ram) {
        const auto checked=writable(address,24,ram);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto offset=*ram_offset(address,4,ram.size());
        auto existing=queues_.find(offset);
        if (existing!=queues_.end()) {
            if (existing->second.published || !existing->second.servers.empty()) return {};
        } else if (queues_.size()>=128 || overlaps(offset,24)) return {};
        if (!order_.empty()) {
            const auto tail=writable(order_.back()+20,4,ram);
            if (tail.status!=NativeServiceStatus::returned) return tail;
        }
        order_.reserve(order_.size()+1);
        if (existing==queues_.end()) queues_.emplace(offset,Queue{address});
        else { existing->second.address=address; existing->second.published=true; }
        for (unsigned word=0;word<6;++word) store_native_word(ram,offset+word*4,word ? 0 : thread);
        if (!order_.empty()) store_native_word(ram,order_.back()+20,address);
        order_.push_back(offset);
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply register_server(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto& a=request.arguments;
        const auto checked=writable(a[0],68,ram);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (request.stack_pointer>0xffffffffU-28) return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto stack=ram_offset(request.stack_pointer+16,4,ram.size());
        if (!stack || ram.size()-*stack<12 || 0x200000-*stack<12)
            return {NativeServiceStatus::address_fault,0,request.stack_pointer};
        const auto cfunc=load_native_word(ram,*stack),cbuf=load_native_word(ram,*stack+4),queue_address=load_native_word(ram,*stack+8);
        const auto compiled=[](std::uint32_t address) {
            const auto code=ram_offset(address,4,0x200000);
            return address && code && nt_iop_native_instruction(*code);
        };
        if (!compiled(a[2])) return {NativeServiceStatus::unmapped_code,0,a[2]};
        if (cfunc && !compiled(cfunc)) return {NativeServiceStatus::unmapped_code,0,cfunc};
        const auto queue_offset=ram_offset(queue_address,4,ram.size());
        if (!queue_offset) return {NativeServiceStatus::address_fault,0,queue_address};
        const auto found=queues_.find(*queue_offset);
        if (found==queues_.end()) return {};
        const auto offset=*ram_offset(a[0],4,ram.size());
        if (servers_.size()>=1024 || overlaps(offset,68)) return {};
        auto& queue=found->second;
        const auto link=queue.servers.empty() ? *queue_offset+8 : queue.servers.back()+56;
        const auto link_check=writable(link,4,ram);
        if (link_check.status!=NativeServiceStatus::returned) return link_check;
        queue.servers.reserve(queue.servers.size()+1);
        servers_.emplace(offset,Server{a[0],*queue_offset,request.global_pointer});
        const std::array<std::pair<unsigned,std::uint32_t>,8> fields{{
            {0,a[1]},{4,a[2]},{8,a[3]},{16,cfunc},{20,cbuf},{56,0},{60,0},{64,queue_address}}};
        for (const auto [at,value] : fields) store_native_word(ram,offset+at,value);
        store_native_word(ram,link,a[0]); queue.servers.push_back(offset);
        return {NativeServiceStatus::returned};
    }
    NativeSifCommandEndpoint* peer_;
    NativeServices* workers_;
    std::map<std::uint32_t,Queue> queues_;
    std::map<std::uint32_t,Server> servers_;
    std::vector<std::uint32_t> order_;
    std::map<const Cpu*,Invocation> invocations_;
};
}
