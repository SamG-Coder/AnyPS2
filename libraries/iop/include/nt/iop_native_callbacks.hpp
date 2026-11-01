#pragma once
#include <nt/iop_native.hpp>
#include <cstdint>
#include <optional>
#include <vector>
#include <stdexcept>
namespace nt::iop {
struct NativeCallback {
    std::uint32_t address,argument;
    std::optional<std::uint32_t> second_argument;
    std::optional<std::uint32_t> global_pointer;
    std::optional<std::uint32_t> third_argument,fourth_argument;
};
class NativeCallbackSource {
public:
    virtual ~NativeCallbackSource()=default;
    virtual std::optional<NativeCallback> take()=0;
    virtual void finish(std::uint32_t result)=0;
    virtual std::optional<NativeResult> failure() const { return {}; }
};
// Fair selection among native completion sources; an active callback retains
// its source across budget boundaries until its compiled return completes.
class NativeCallbackMux final : public NativeCallbackSource {
public:
    explicit NativeCallbackMux(std::vector<NativeCallbackSource*> sources) : sources_(std::move(sources)) {
        for (const auto source : sources_) if (!source) throw std::invalid_argument("Null native callback source");
    }
    std::optional<NativeCallback> take() override {
        if (active_ || failure()) return {};
        failed_=nullptr;
        for (std::size_t count=0;count<sources_.size();++count) {
            const auto index=(next_+count)%sources_.size();
            const auto callback=sources_[index]->take();
            if (sources_[index]->failure()) { failed_=sources_[index]; return {}; }
            if (callback) {
                active_=sources_[index]; next_=(index+1)%sources_.size(); return callback;
            }
        }
        return {};
    }
    std::optional<NativeResult> failure() const override { return failed_ ? failed_->failure() : std::nullopt; }
    void finish(std::uint32_t result) override {
        if (!active_) throw std::logic_error("Native callback mux has no active source");
        active_->finish(result); active_=nullptr;
    }
private:
    std::vector<NativeCallbackSource*> sources_;
    NativeCallbackSource* active_=nullptr;
    NativeCallbackSource* failed_=nullptr;
    std::size_t next_=0;
};

}
