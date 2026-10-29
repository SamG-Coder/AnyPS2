#pragma once
#include <nt/iop_startup.hpp>
#include <nt/iop_intrman.hpp>
#include <nt/iop_dmacman.hpp>
#include <nt/iop_heaplib.hpp>
#include <memory>

namespace nt::iop {
// Partial disc reboot context. Its RAM remains private until the complete
// service startup sequence is implemented; memory startup alone is not BOOTEND.
class Reboot {
public:
    explicit Reboot(ByteView image,Sbus* sbus=nullptr);
    Reboot(const Reboot&)=delete;
    Reboot& operator=(const Reboot&)=delete;
    StartupResult advance(std::uint64_t budget);
    void prepare_manager();
    StartupResult run_manager(std::uint64_t budget);
    const std::optional<elf::IopModule>& manager() const { return manager_; }
    std::uint64_t executed() const { return call_->cpu().executed+service_executed_+(manager_call_ ? manager_call_->cpu().executed : 0); }
    ByteView memory() const { return ram_; }
    std::uint32_t pending_interrupts() const { return devices_.pending_sources(); }
    const Cpu* manager_cpu() const { return manager_call_ ? &manager_call_->cpu() : nullptr; }
    bool receive_sif1(std::deque<std::uint32_t>& fifo) { return devices_.receive_sif1(fifo,ram_); }
    bool send_sif0(std::deque<std::uint32_t>& fifo) { return devices_.send_sif0(fifo,ram_); }
    bool intrman_registered() const { return intrman_ && le32(ram_,intrman_->registration_flag())==1; }
    bool dmacman_registered() const { return dmacman_ && le32(ram_,dmacman_->registration_flag())==1; }
    bool heaplib_registered() const { return heaplib_ && le32(ram_,heaplib_->registration_flag())==1; }
private:
    Bytes image_, ram_;
    elf::IopModule memory_module_;
    Libraries libraries_;
    BootstrapDevices devices_;
    std::unique_ptr<Intrman> intrman_;
    std::unique_ptr<Dmacman> dmacman_;
    std::unique_ptr<Heaplib> heaplib_;
    CallRouter calls_;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> allocations_;
    std::unique_ptr<Startup> call_;
    std::optional<elf::IopModule> manager_;
    std::uint64_t service_executed_=0;
    std::uint32_t allocator_=0, memory_exports_=0;
    std::uint32_t releaser_=0, block_size_=0, allocator_stack_=0;
    std::unique_ptr<Startup> manager_call_;
    std::uint32_t memory_call(std::uint32_t entry,EntryRegisters arguments);
    std::uint32_t allocate_region(std::uint32_t size,unsigned policy=0,bool required=true);
    HeapRegion allocate_heap_region(std::uint32_t size,unsigned policy);
    bool release_heap_region(std::uint32_t base);
    void prepare_handoff();
};
}
