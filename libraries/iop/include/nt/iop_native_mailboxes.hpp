#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_waits.hpp>
#include <algorithm>
#include <map>
#include <vector>

namespace nt::iop {
// Native ownership defines queue order; public message links are mirrored data,
// never pointers followed by the host scheduler.
class NativeMailboxServices final : public NativeServices, public NativeWaitQueue {
public:
    explicit NativeMailboxServices(NativeThreadWaits* waits=nullptr) : waits_(waits) {}
    bool cancel_wait(std::uint32_t object,std::uint32_t thread) override {
        const auto found=boxes_.find(object);
        if (found==boxes_.end()) return false;
        return std::erase_if(found->second.waiters,[&](const auto& waiter) { return waiter.thread==thread; })!=0;
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thmsgbx) return {};
        const auto op=request.selector;
        if (!((op>=4 && op<=9) || op==11 || op==12)) return {};
        if (request.interrupt_context!=(op==7 || op==12)) return returned(-100);
        const auto& a=request.arguments;
        if (op==4) {
            const auto offset=ram_offset(a[0],4,ram.size());
            if (!offset || ram.size()-*offset<8 || 0x200000-*offset<8) return {NativeServiceStatus::address_fault,0,a[0]};
            const auto attr=load_native_word(ram,*offset);
            if (attr&~5U) return returned(-401);
            if (boxes_.size()>=4096 || next_id_>0x7fffffffU) return returned(-400);
            boxes_.emplace(next_id_,Box{attr,load_native_word(ram,*offset+4)});
            return returned(static_cast<std::int32_t>(next_id_++));
        }
        const auto id=op==8 || op==9 ? a[1] : a[0];
        const auto found=boxes_.find(id);
        if (found==boxes_.end()) return returned(-410);
        auto& box=found->second;
        if (op==5) {
            const bool wake=!box.waiters.empty();
            for (const auto& waiter : box.waiters) waits_->resume_wait(waiter.thread,-425);
            boxes_.erase(found);
            return wake && waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
        }
        if (op==6 || op==7) {
            if (!box.waiters.empty()) {
                auto selected=box.waiters.begin();
                if (box.attributes&1) selected=std::min_element(selected,box.waiters.end(),[&](const auto& left,const auto& right) {
                    return waits_->wait_priority(left.thread)<waits_->wait_priority(right.thread);
                });
                if (selected->output) {
                    const auto checked=writable(ram,selected->output,4);
                    if (checked.status!=NativeServiceStatus::returned) return checked;
                }
                waits_->resume_wait(selected->thread,0);
                if (selected->output) store_native_word(ram,*ram_offset(selected->output,4,ram.size()),a[1]);
                box.waiters.erase(selected);
                return op==6 && waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
            }
            const auto checked=writable(ram,a[1],8);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            const auto address=*ram_offset(a[1],4,ram.size());
            for (const auto& [other_id,other] : boxes_) for (const auto message : other.messages)
                if ((message&0x1fffffffU)<address+8 && address<(message&0x1fffffffU)+8) return {}; // Already owned, including aliases.
            if (box.messages.size()>=4096) return {};
            for (const auto message : box.messages) {
                const auto valid=writable(ram,message,8);
                if (valid.status!=NativeServiceStatus::returned) return valid;
            }
            auto messages=box.messages;
            auto place=messages.end();
            if (box.attributes&4) {
                const auto priority=ram[address+4];
                place=std::find_if(messages.begin(),messages.end(),[&](auto message) { return ram[(message&0x1fffffffU)+4]>priority; });
            }
            messages.insert(place,a[1]);
            for (std::size_t index=0;index<messages.size();++index)
                store_native_word(ram,messages[index]&0x1fffffffU,messages[(index+1)%messages.size()]);
            box.messages.swap(messages); return returned();
        }
        if (op==8 || op==9) {
            if (box.messages.empty()) {
                if (op==9) return returned(-424);
                const auto thread=waits_ ? waits_->current_waiter() : std::nullopt;
                if (!thread) return {};
                if (a[0]) {
                    const auto checked=writable(ram,a[0],4);
                    if (checked.status!=NativeServiceStatus::returned) return checked;
                }
                box.waiters.push_back({*thread,a[0]});
                waits_->suspend_wait(*thread,id,NativeWaitKind::mailbox,*this);
                return {NativeServiceStatus::yielded};
            }
            const auto checked=writable(ram,a[0],4);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            if (box.messages.size()>1) {
                const auto valid=writable(ram,box.messages.back(),4);
                if (valid.status!=NativeServiceStatus::returned) return valid;
                store_native_word(ram,box.messages.back()&0x1fffffffU,box.messages[1]);
            }
            store_native_word(ram,*ram_offset(a[0],4,ram.size()),box.messages.front());
            box.messages.erase(box.messages.begin()); return returned();
        }
        const auto checked=writable(ram,a[1],28);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const std::array<std::uint32_t,5> fields{box.attributes,box.option,static_cast<std::uint32_t>(box.waiters.size()),
            static_cast<std::uint32_t>(box.messages.size()),box.messages.empty() ? 0 : box.messages.front()};
        const auto offset=*ram_offset(a[1],4,ram.size());
        for (unsigned index=0;index<fields.size();++index) store_native_word(ram,offset+index*4,fields[index]);
        return returned();
    }
private:
    static NativeServiceReply returned(std::int32_t value=0) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    static NativeServiceReply writable(std::span<std::uint8_t> ram,std::uint32_t address,std::uint32_t size) {
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset) return {NativeServiceStatus::address_fault,0,address};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,address};
        return returned();
    }
    struct Waiter { std::uint32_t thread,output; };
    struct Box {
        std::uint32_t attributes,option;
        std::vector<std::uint32_t> messages;
        std::vector<Waiter> waiters;
    };
    NativeThreadWaits* waits_;
    std::map<std::uint32_t,Box> boxes_;
    std::uint32_t next_id_=1;
};
}
