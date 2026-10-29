#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_waits.hpp>
#include <algorithm>
#include <map>
#include <vector>

namespace nt::iop {
class NativeSemaphoreServices final : public NativeServices {
public:
    explicit NativeSemaphoreServices(NativeThreadWaits* waits=nullptr) : waits_(waits) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thsemap) return {};
        const auto op=request.selector;
        if (!((op>=4 && op<=9) || op==11 || op==12)) return {};
        if (request.interrupt_context!=(op==7 || op==12)) return returned(-100);
        const auto& a=request.arguments;
        if (op==4) {
            const auto offset=ram_offset(a[0],4,ram.size());
            if (!offset || ram.size()-*offset<16 || 0x200000-*offset<16) return {NativeServiceStatus::address_fault,0,a[0]};
            const auto attr=load_native_word(ram,*offset),initial=load_native_word(ram,*offset+8),maximum=load_native_word(ram,*offset+12);
            if (attr&~0x101U) return returned(-401);
            // Priority inheritance needs ownership tracking beyond a counting semaphore.
            if ((attr&0x100) || !maximum || maximum>0x7fffffffU || initial>maximum) return {};
            if (semaphores_.size()>=4096 || next_id_>0x7fffffffU) return returned(-400);
            semaphores_.emplace(next_id_,Semaphore{attr,load_native_word(ram,*offset+4),initial,maximum,initial});
            return returned(static_cast<std::int32_t>(next_id_++));
        }
        const auto found=semaphores_.find(a[0]);
        if (found==semaphores_.end()) return returned(-408);
        auto& semaphore=found->second;
        if (op==5) {
            const bool wake=!semaphore.waiters.empty();
            for (const auto thread : semaphore.waiters) waits_->resume_wait(thread,-425);
            semaphores_.erase(found);
            return wake && waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
        }
        if (op==6 || op==7) {
            if (!semaphore.waiters.empty()) {
                auto selected=semaphore.waiters.begin();
                if (semaphore.attributes&1) selected=std::min_element(selected,semaphore.waiters.end(),[&](auto left,auto right) {
                    return waits_->wait_priority(left)<waits_->wait_priority(right);
                });
                waits_->resume_wait(*selected,0); semaphore.waiters.erase(selected);
                return op==6 && waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
            }
            if (semaphore.current==semaphore.maximum) return returned(-420);
            ++semaphore.current; return returned();
        }
        if (op==8 || op==9) {
            if (semaphore.current) { --semaphore.current; return returned(); }
            if (op==9) return returned(-419);
            const auto thread=waits_ ? waits_->current_waiter() : std::nullopt;
            if (!thread) return {};
            semaphore.waiters.push_back(*thread);
            waits_->suspend_wait(*thread,a[0],NativeWaitKind::semaphore);
            return {NativeServiceStatus::yielded};
        }
        const auto offset=ram_offset(a[1],4,ram.size());
        if (!offset || ram.size()-*offset<32 || 0x200000-*offset<32) return {NativeServiceStatus::address_fault,0,a[1]};
        for (auto at=*offset;at<*offset+32;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,a[1]};
        const std::array<std::uint32_t,6> fields{semaphore.attributes,semaphore.option,semaphore.initial,
            semaphore.maximum,semaphore.current,static_cast<std::uint32_t>(semaphore.waiters.size())};
        for (unsigned index=0;index<fields.size();++index) store_native_word(ram,*offset+index*4,fields[index]);
        return returned();
    }
private:
    static NativeServiceReply returned(std::int32_t value=0) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    struct Semaphore {
        std::uint32_t attributes,option,initial,maximum,current;
        std::vector<std::uint32_t> waiters;
    };
    NativeThreadWaits* waits_;
    std::map<std::uint32_t,Semaphore> semaphores_;
    std::uint32_t next_id_=1;
};
}
