#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nt::iop {
struct Cpu;
enum class CallState { unsupported, returned, address_fault, transferred, pending };
struct CallResult {
    CallState state=CallState::unsupported;
    std::uint32_t value=0, address=0;
};
// Native invocation is restricted to installed entry addresses. Guest syscall
// transitions use a separate ABI hook; unknown services remain unsupported.
class HostCalls {
public:
    virtual ~HostCalls()=default;
    virtual bool owns(std::uint32_t,std::size_t) const { return false; }
    virtual CallResult invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& arguments,
                              std::span<std::uint8_t> ram,bool interrupt_context)=0;
    // Optional documented guest syscall ABI. Providers validate the syscall
    // and prepare a complete CPU transition; unsupported calls change nothing.
    virtual CallResult syscall(const Cpu&,Cpu&,std::span<std::uint8_t>) { return {}; }
    virtual CallResult interrupt(const Cpu&,Cpu&,std::span<std::uint8_t>) { return {}; }
};
class CallRouter final : public HostCalls {
public:
    // Providers must outlive the router and use disjoint guest allocations.
    void add(HostCalls& provider) { providers_.push_back(&provider); }
    CallResult invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& arguments,
                      std::span<std::uint8_t> ram,bool interrupt_context) override {
        for (auto* provider : providers_) {
            const auto result=provider->invoke(pc,arguments,ram,interrupt_context);
            if (result.state!=CallState::unsupported) return result;
        }
        return {};
    }
    CallResult syscall(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) override;
    CallResult interrupt(const Cpu& cpu,Cpu& next,std::span<std::uint8_t> ram) override {
        for (auto* provider : providers_) {
            const auto result=provider->interrupt(cpu,next,ram);
            if (result.state!=CallState::unsupported) return result;
        }
        return {};
    }
private:
    std::vector<HostCalls*> providers_;
};
}
