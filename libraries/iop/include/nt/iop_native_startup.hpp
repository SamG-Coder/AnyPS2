#pragma once
#include <nt/iop_native_context.hpp>

namespace nt::iop {
// Resumable AOT module initialization. Finishing entries and boot callbacks
// does not imply worker readiness, peer synchronization or reset completion.
class NativeModuleStartup {
public:
    struct Returned { std::uint32_t value; std::uint64_t executed; };
    NativeModuleStartup(NativeEntry execute,NativeServiceContext& services,std::span<std::uint8_t> ram,
                        NativeModule image,std::vector<NativeModule> entries,std::uint32_t stack_top)
        : execute_(execute),services_(services),ram_(ram),entries_(std::move(entries)),stack_top_(stack_top),
          initial_executed_(services.threads.executed()) {
        if (!execute || entries_.empty() || stack_top%16 || stack_top<16 || stack_top>ram.size() ||
            image.base>ram.size() || image.memory_size>ram.size()-image.base || image.initialized_size>image.memory_size)
            throw std::invalid_argument("Invalid native module startup context");
        for (const auto& entry:entries_) {
            if (entry.base<image.base || entry.base-image.base>image.memory_size ||
                entry.memory_size>image.memory_size-(entry.base-image.base) || entry.initialized_size>entry.memory_size ||
                entry.entry<entry.base || entry.entry-entry.base>=entry.initialized_size ||
                entry.entry%4 || (entry.kind!=NativeModuleKind::compiled && entry.kind!=NativeModuleKind::archive) ||
                (entry.kind==NativeModuleKind::compiled && !nt_iop_native_instruction(entry.entry)))
                throw std::invalid_argument("Invalid or uncompiled native startup entry");
        }
        returned_.reserve(entries_.size());
        callback_state_.registers[29]=stack_top;
    }
    NativeModuleStartup(const NativeModuleStartup&)=delete;
    NativeModuleStartup& operator=(const NativeModuleStartup&)=delete;
    bool finished() const { return returned_.size()==entries_.size() && services_.boot_callbacks.complete(); }
    std::span<const Returned> returned() const { return returned_; }
    std::uint64_t executed() const { return services_.threads.executed()-initial_executed_+callback_state_.executed; }
    std::uint64_t callback_operations() const { return callback_state_.executed; }
    NativeResult advance(std::uint64_t budget) {
        NativeResult result{NativeStop::budget,active_ ? services_.threads.state(*active_).pc : callback_state_.pc};
        while (budget && !finished()) {
            const auto before=executed();
            if (returned_.size()<entries_.size()) {
                if (entries_[returned_.size()].kind==NativeModuleKind::archive) {
                    if (!services_.archives.initialize()) return {NativeStop::import_request,entries_[returned_.size()].entry};
                    returned_.push_back({0,0});
                    continue;
                }
                if (!active_) {
                    const auto& entry=entries_[returned_.size()];
                    Cpu cpu; cpu.pc=entry.entry; cpu.registers[28]=entry.global_pointer;
                    cpu.registers[29]=stack_top_; cpu.registers[31]=return_address;
                    active_=services_.threads.admit(cpu);
                }
                result=services_.threads.run(execute_,ram_,budget,services_);
                const auto& state=services_.threads.state(*active_);
                if (result.reason==NativeStop::unmapped_code && state.pc==return_address && !state.delay_slot) {
                    returned_.push_back({state.registers[2],state.executed}); active_.reset();
                } else if (result.reason!=NativeStop::unmapped_code || result.pc!=return_address) return result;
            } else {
                result=services_.boot_callbacks.finish_startup(execute_,callback_state_,ram_,budget,services_);
                if (!services_.boot_callbacks.complete()) return result;
            }
            const auto retired=executed()-before;
            if (retired>budget) throw std::logic_error("Native startup exceeded its operation budget");
            budget-=retired;
        }
        return finished() ? NativeResult{NativeStop::unmapped_code,return_address} : NativeResult{NativeStop::budget,result.pc};
    }
    static constexpr std::uint32_t return_address=0xfffffffc;
private:
    NativeEntry execute_;
    NativeServiceContext& services_;
    std::span<std::uint8_t> ram_;
    std::vector<NativeModule> entries_;
    std::uint32_t stack_top_;
    std::uint64_t initial_executed_;
    std::optional<std::uint32_t> active_;
    std::vector<Returned> returned_;
    Cpu callback_state_;
};
}
