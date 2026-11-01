#pragma once
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_waits.hpp>
#include <deque>
#include <map>
#include <vector>

namespace nt::iop {
// Native ownership of fixed-size allocations. The payload is ordinary program
// data; free lists, allocation flags and waiting continuations stay on the host.
class NativeFixedPoolServices final : public NativeServices, public NativeWaitQueue {
public:
    NativeFixedPoolServices(NativeHeapServices& heap,NativeThreadWaits* waits=nullptr) : heap_(heap),waits_(waits) {}
    bool cancel_wait(std::uint32_t object,std::uint32_t thread) override {
        const auto pool=pools_.find(object);
        return pool!=pools_.end() && std::erase(pool->second.waiters,thread)!=0;
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thfpool) return {};
        const auto op=request.selector;
        if (!((op>=4 && op<=9) || op==11 || op==12)) return {};
        if (request.interrupt_context!=(op==8 || op==12)) return returned(-100);
        const auto& a=request.arguments;
        if (op==4) return create(a[0],ram);
        const auto found=pools_.find(a[0]);
        if (found==pools_.end()) return returned(-412);
        auto& pool=found->second;
        if (op==5) {
            const auto released=heap_.invoke({5,{pool.address},NativeServiceKind::sysmem},ram);
            if (released.status!=NativeServiceStatus::returned || released.value) return {NativeServiceStatus::unavailable};
            const bool wake=!pool.waiters.empty();
            for (const auto waiter:pool.waiters) waits_->resume_wait(waiter,-425);
            pools_.erase(found);
            return wake && waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
        }
        if (op>=6 && op<=8) {
            if (!pool.free.empty()) {
                const auto index=pool.free.front(); pool.free.pop_front(); pool.used[index]=true;
                return returned(pool.address+index*pool.stride);
            }
            if (op!=6) return returned(-400);
            const auto waiter=waits_ ? waits_->current_waiter() : std::nullopt;
            if (!waiter) return {NativeServiceStatus::unavailable};
            pool.waiters.push_back(*waiter);
            waits_->suspend_wait(*waiter,a[0],NativeWaitKind::fixed_pool,*this);
            return {NativeServiceStatus::yielded};
        }
        if (op==9) {
            const auto offset=ram_offset(a[1],4,ram.size());
            if (!offset || *offset<pool.address) return returned(-426);
            const auto displacement=*offset-pool.address,index=displacement/pool.stride;
            if (displacement%pool.stride || index>=pool.used.size() || !pool.used[index]) return returned(-426);
            if (!pool.waiters.empty()) {
                auto selected=pool.waiters.begin();
                if (pool.attributes&1) selected=std::min_element(selected,pool.waiters.end(),[&](auto left,auto right) {
                    return waits_->wait_priority(left)<waits_->wait_priority(right);
                });
                waits_->resume_wait(*selected,static_cast<std::int32_t>(*offset)); pool.waiters.erase(selected);
                return waits_->current_waiter() ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
            }
            pool.used[index]=false; pool.free.push_back(index); return returned();
        }
        const auto offset=ram_offset(a[1],4,ram.size());
        if (!offset || 40>ram.size()-*offset || 40>0x200000-*offset) return {NativeServiceStatus::address_fault,0,a[1]};
        for (auto at=*offset;at<*offset+24;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,a[1]};
        const std::array<std::uint32_t,6> fields{pool.attributes,pool.option,pool.block_size,
            static_cast<std::uint32_t>(pool.used.size()),static_cast<std::uint32_t>(pool.free.size()),
            static_cast<std::uint32_t>(pool.waiters.size())};
        for (unsigned i=0;i<fields.size();++i) store_native_word(ram,*offset+i*4,fields[i]);
        return returned();
    }
private:
    struct Pool {
        std::uint32_t address,attributes,option,block_size,stride;
        std::deque<std::uint32_t> free;
        std::vector<bool> used;
        std::vector<std::uint32_t> waiters;
    };
    static NativeServiceReply returned(std::int32_t value=0) { return {NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; }
    NativeServiceReply create(std::uint32_t pointer,std::span<std::uint8_t> ram) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || 16>ram.size()-*offset || 16>0x200000-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        const auto attr=load_native_word(ram,*offset),option=load_native_word(ram,*offset+4);
        const auto size=load_native_word(ram,*offset+8),count=load_native_word(ram,*offset+12);
        if (attr&~0x201U) return returned(-401);
        if (!size || size>0x200000 || !count || count>0x80000) return returned(-427);
        const auto stride=(size+3)&~3U;
        if (count>0x200000/stride) return returned(-427);
        if (pools_.size()>=4096 || next_id_>0x7fffffff) return returned(-400);
        Pool pool{0,attr,option,size,stride};
        pool.used.resize(count,false);
        for (std::uint32_t index=0;index<count;++index) pool.free.push_back(index);
        const auto allocation=heap_.invoke({4,{(attr>>9)&1U,stride*count},NativeServiceKind::sysmem},ram);
        if (allocation.status!=NativeServiceStatus::returned) return allocation;
        if (!allocation.value) return returned(-400);
        pool.address=allocation.value;
        try { pools_.emplace(next_id_,std::move(pool)); }
        catch (...) { heap_.invoke({5,{allocation.value},NativeServiceKind::sysmem},ram); throw; }
        return returned(static_cast<std::int32_t>(next_id_++));
    }
    NativeHeapServices& heap_;
    NativeThreadWaits* waits_;
    std::map<std::uint32_t,Pool> pools_;
    std::uint32_t next_id_=1;
};
}
