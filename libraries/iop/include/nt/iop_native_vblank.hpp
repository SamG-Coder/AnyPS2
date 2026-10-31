#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iop_native_code.hpp>
#include <algorithm>
#include <bit>
#include <deque>
#include <vector>

namespace nt::iop {
// Display edges are supplied by the native display owner, never by instruction
// counts. A queued invocation holds its identity until compiled return.
class NativeVblankServices final : public NativeServices, public NativeCallbackSource {
public:
    void overflow() { failed_=NativeResult{NativeStop::overflow,0,0,0x140008}; }
    NativeServiceReply invoke(const NativeServiceRequest& r,std::span<std::uint8_t>) override {
        if (r.kind!=NativeServiceKind::vblank || (r.selector!=8 && r.selector!=9)) return {};
        const auto result=[](int value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (r.interrupt_context) return result(-100);
        const bool end=r.arguments[0]!=0;
        const auto address=r.arguments[r.selector==8 ? 2 : 1];
        const auto found=std::find_if(handlers_.begin(),handlers_.end(),[&](const auto& h) { return h.end==end && h.address==address; });
        if (r.selector==9) {
            if (found==handlers_.end()) return result(-105);
            const auto id=found->id;
            handlers_.erase(found);
            std::erase_if(pending_,[&](const auto& h) { return h.id==id; });
            return result();
        }
        if (found!=handlers_.end()) return result(-104);
        if (handlers_.size()>=16) return result(-400);
        const auto target=ram_offset(address,4,0x200000);
        if (!address || !target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,address};
        handlers_.push_back({++identity_,end,std::bit_cast<std::int32_t>(r.arguments[1]),address,r.arguments[3]});
        std::stable_sort(handlers_.begin(),handlers_.end(),[](const auto& a,const auto& b) { return a.priority<b.priority; });
        return result();
    }
    void post(bool end) {
        for (const auto& h:handlers_) if (h.end==end) {
            if (pending_.size()>=256) { overflow(); return; }
            pending_.push_back(h);
        }
    }
    std::optional<NativeCallback> take() override {
        if (active_ || pending_.empty() || failed_) return {};
        active_=pending_.front(); pending_.pop_front();
        return NativeCallback{active_->address,active_->argument};
    }
    void finish(std::uint32_t result) override {
        if (!active_) throw std::logic_error("Vblank completion without an active callback");
        if (!result) {
            const auto id=active_->id;
            std::erase_if(handlers_,[&](const auto& h) { return h.id==id; });
            std::erase_if(pending_,[&](const auto& h) { return h.id==id; });
        }
        active_.reset();
    }
    std::optional<NativeResult> failure() const override { return failed_; }
private:
    struct Handler { std::uint64_t id; bool end; std::int32_t priority; std::uint32_t address,argument; };
    std::vector<Handler> handlers_;
    std::deque<Handler> pending_;
    std::optional<Handler> active_;
    std::optional<NativeResult> failed_;
    std::uint64_t identity_=0;
};
}
