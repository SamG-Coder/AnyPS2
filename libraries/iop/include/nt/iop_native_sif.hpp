#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_callbacks.hpp>
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>

namespace nt::iop {
struct NativeSifDestination {
    std::span<std::uint8_t> memory;
    // The converted EE owner must reject writes to its compiled instruction ranges.
    std::function<bool(std::uint32_t,std::uint32_t)> writable;
};
// Direct data transfer, not a DMA engine. An explicit converted EE data endpoint
// is required; the standalone IOP probe cannot fabricate an attached peer.
class NativeSifTransfers final : public NativeServices, public NativeCallbackSource {
public:
    explicit NativeSifTransfers(NativeSifDestination* destination=nullptr) : destination_(destination) {
        if (destination && (destination->memory.size()>0x2000000 || !destination->writable))
            throw std::invalid_argument("Invalid native SIF destination");
    }
    // Teardown is serialized with completion delivery. Never discard an active
    // callback whose native execution still owns the interrupt context.
    bool stop() {
        if (active_) return false;
        stopped_=true; completions_.clear(); handler_.reset(); return true;
    }
    bool accepting() const { return destination_ && !stopped_; }
    // Shared commit boundary for raw transfers and native command sends. Reserve
    // completion storage before invoking a nonthrowing, already validated write.
    template<class Commit>
    NativeServiceReply complete(Commit&& commit,std::optional<NativeCallback> callback={}) {
        static_assert(noexcept(commit()), "SIF commit must not throw after writes begin");
        if (!accepting() || next_token_>0x7fffffffU) return {};
        if (callback && !compiled(callback->address)) return {NativeServiceStatus::unmapped_code,0,callback->address};
        if (completions_.size()+1+static_cast<unsigned>(callback.has_value())>1024)
            return {NativeServiceStatus::returned};
        auto pending=completions_;
        pending.push_back({{},true});
        if (callback) pending.push_back({*callback,false});
        commit();
        completions_.swap(pending);
        return {NativeServiceStatus::returned,next_token_++};
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sifman || !destination_ || stopped_) return {};
        const auto op=request.selector;
        const auto& a=request.arguments;
        const auto returned=[](std::uint32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,value}; };
        if (op==8) return returned(0xffffffff); // Accepted transfers finish before their completion is queued.
        if (op==31) { handler_.reset(); return returned(); }
        if (op==30) {
            if (!a[0]) { handler_.reset(); return returned(); }
            if (!compiled(a[0])) return {NativeServiceStatus::unmapped_code,0,a[0]};
            handler_=NativeCallback{a[0],a[1]}; return returned();
        }
        if (op!=7 && op!=32) return {};
        const auto count=a[1];
        if (!count || count>32 || completions_.size()+2>1024) return returned();
        if (op==32 && a[2] && !compiled(a[2])) return {NativeServiceStatus::unmapped_code,0,a[2]};
        const auto descriptors=ram_offset(a[0],4,ram.size());
        if (!descriptors || count*16>ram.size()-*descriptors || count*16>0x200000-*descriptors)
            return {NativeServiceStatus::address_fault,0,a[0]};
        struct Transfer { std::uint32_t destination,offset,size; };
        std::vector<Transfer> transfers; transfers.reserve(count);
        std::vector<std::uint8_t> payload;
        for (unsigned index=0;index<count;++index) {
            const auto at=*descriptors+index*16;
            const auto source=load_native_word(ram,at),address=load_native_word(ram,at+4);
            const auto size=load_native_word(ram,at+8),attributes=load_native_word(ram,at+12);
            // Irregular tail lanes and EE interrupt/tag attributes need an EE
            // command endpoint; do not invent their effects with a plain copy.
            if ((attributes&~2U) || !size || (size&15) || (source&15) || (address&15) || size>0x100000-payload.size()) return {};
            const auto input=ram_offset(source,1,ram.size());
            if (!input || size>ram.size()-*input || size>0x200000-*input) return {NativeServiceStatus::address_fault,0,source};
            const auto output=address&0x1fffffffU;
            if (output>destination_->memory.size() || size>destination_->memory.size()-output)
                return {NativeServiceStatus::address_fault,0,address};
            if (!destination_->writable(output,size)) return {NativeServiceStatus::code_write,0,address};
            transfers.push_back({output,static_cast<std::uint32_t>(payload.size()),size});
            payload.insert(payload.end(),ram.begin()+*input,ram.begin()+*input+size);
        }
        const auto callback=op==32 && a[2] ? std::optional<NativeCallback>{{a[2],a[3]}} : std::nullopt;
        return complete([&]() noexcept {
            for (const auto& transfer : transfers)
                std::copy_n(payload.begin()+transfer.offset,transfer.size,destination_->memory.begin()+transfer.destination);
        },callback);
    }
    std::optional<NativeCallback> take() override {
        if (active_) return {};
        while (!completions_.empty()) {
            const auto pending=completions_.front(); completions_.pop_front();
            const auto callback=pending.general ? handler_ : std::optional<NativeCallback>{pending.callback};
            if (!callback) continue;
            active_=true; return callback;
        }
        return {};
    }
    void finish(std::uint32_t) override {
        if (!active_) throw std::logic_error("Native SIF completion has no active callback");
        active_=false; // Completion callbacks return void; their v0 never reschedules them.
    }
private:
    static bool compiled(std::uint32_t address) {
        const auto entry=ram_offset(address,4,0x200000);
        return entry && nt_iop_native_instruction(*entry);
    }
    NativeSifDestination* destination_;
    std::optional<NativeCallback> handler_;
    struct Completion { NativeCallback callback; bool general; };
    std::deque<Completion> completions_;
    std::uint32_t next_token_=1;
    bool active_=false,stopped_=false;
};
}
