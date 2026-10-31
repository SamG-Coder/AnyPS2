#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_events.hpp>
#include <algorithm>
#include <bit>
#include <deque>
#include <vector>

namespace nt::iop {
// Display edges are supplied by the native display owner, never by instruction
// counts. A queued invocation holds its identity until compiled return.
class NativeVblankServices final : public NativeServices, public NativeCallbackSource {
public:
    void connect(NativeEventServices& events,std::span<std::uint8_t> ram) {
        if (events_) throw std::logic_error("Vblank event owner already connected");
        const auto event=events.create(2,0,0);
        if (event<0) throw std::runtime_error("Cannot allocate native vblank event");
        events_=&events; event_=static_cast<std::uint32_t>(event); ram_=ram;
    }
    void overflow() { failed_=NativeResult{NativeStop::overflow,0,0,0x140008}; }
    NativeServiceReply invoke(const NativeServiceRequest& r,std::span<std::uint8_t>) override {
        if (r.kind!=NativeServiceKind::vblank || r.selector<4 || r.selector>9) return {};
        const auto result=[](int value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (r.interrupt_context) return result(-100);
        if (r.selector<=7) {
            if (!events_) return {NativeServiceStatus::unavailable};
            constexpr std::uint32_t bits[]{1,4,2,8};
            auto wait=r;
            wait.kind=NativeServiceKind::thevent; wait.selector=10;
            wait.arguments={event_,bits[r.selector-4],1,0};
            auto reply=events_->invoke(wait,ram_);
            if (reply.status==NativeServiceStatus::unsupported) reply.status=NativeServiceStatus::unavailable;
            return reply;
        }
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
        if (failed_) return;
        const auto count=std::count_if(handlers_.begin(),handlers_.end(),[&](const auto& h) { return h.end==end; });
        if (pending_.size()+count+(events_ ? 1U : 0U)>256) { overflow(); return; }
        bool publish=events_!=nullptr;
        for (const auto& h:handlers_) if (h.end==end) {
            if (publish && h.priority>=128) {
                pending_.push_back({0,end,128,0,0}); publish=false;
            }
            pending_.push_back(h);
        }
        if (publish) pending_.push_back({0,end,128,0,0});
    }
    std::optional<NativeCallback> take() override {
        if (active_ || failed_) return {};
        while (!pending_.empty()) {
            const auto next=pending_.front(); pending_.pop_front();
            if (!next.id) {
                // Publish only when the interrupt owner consumes this edge, at
                // the base handler's priority. Edge bits wake current waiters
                // then disappear; phase bits remain until the opposite edge.
                const auto change=[&](unsigned op,std::uint32_t bits) {
                    return events_->invoke({op,{event_,bits},NativeServiceKind::thevent,0,true},ram_).status==NativeServiceStatus::returned;
                };
                if (!change(7,next.end ? 4U : 1U) || !change(7,next.end ? 8U : 2U) ||
                    !change(9,next.end ? ~6U : ~9U)) { overflow(); return {}; }
                continue;
            }
            active_=next;
            return NativeCallback{next.address,next.argument};
        }
        return {};
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
    NativeEventServices* events_=nullptr;
    std::uint32_t event_=0;
    std::span<std::uint8_t> ram_;
};
}
