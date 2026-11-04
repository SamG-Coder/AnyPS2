#include <nt/ee_native.hpp>
#include <nt/ee_exception.hpp>
#include <nt/ee_interrupts.hpp>
#include <nt/ee_completions.hpp>
#include <nt/ee_performance.hpp>
#include <nt/ee_graphics.hpp>
#include <thread>
extern "C" std::uint64_t nt_ee_exception_read(nt::ee::Memory* memory,std::uint32_t selector) {
    const auto result=nt::ee::read_exception(*memory,selector);
    return (std::uint64_t(result.status)<<32)|result.value;
}
extern "C" void nt_ee_exception_record(nt::runtime::State* state,nt::ee::Memory* memory,
                                       std::uint32_t code,std::uint32_t address) {
    nt::ee::record_exception(*state,*memory,static_cast<nt::ee::ExceptionCode>(code),address);
}
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
void nt::ee::native_service_wait(std::uint64_t nanoseconds) {
    const auto duration=std::min<std::uint64_t>(nanoseconds,1000000);
    if (!duration) return;
#ifdef _WIN32
    struct Timer {
        HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    thread_local Timer timer;
    if (timer.handle) {
        LARGE_INTEGER due;
        due.QuadPart=-static_cast<LONGLONG>((duration+99)/100);
        if (SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE) &&
            WaitForSingleObject(timer.handle,INFINITE)==WAIT_OBJECT_0) return;
    }
#endif
    std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
}
extern "C" std::uint32_t nt_ee_wait_display(nt::ee::Memory* memory,std::uint32_t address) {
    using namespace nt::ee;
    // Only a proven poll invokes this hint. Recheck the actual native service
    // binding; an identical loop over RAM or another device must not sleep.
    if (nt::runtime::default_ram_address(address)!=0x12001000 || !memory->graphics)
        return static_cast<std::uint32_t>(Stop::budget);
    const auto wait=memory->graphics->display_wait_ns();
    return static_cast<std::uint32_t>(wait && *wait ? Stop::display_wait : Stop::budget);
}
extern "C" std::uint64_t nt_ee_performance(nt::ee::Memory* memory,std::uint32_t operation,
    std::uint32_t counter,std::uint32_t value,std::uint32_t execution_mode) {
    using namespace nt::ee;
    if (operation>static_cast<unsigned>(PerformanceOperation::read_clock) || counter>1)
        return std::uint64_t(Stop::invalid_state)<<32;
    const auto request=static_cast<PerformanceOperation>(operation);
    auto candidate=memory->performance;
    IoRead result{Stop::unsupported_system};
    if (memory->metrics.request) {
        result=memory->metrics.request(memory->metrics.context,candidate,request,counter,value,execution_mode);
    } else {
        // Configuration is observable independently of accumulated measurements.
        // Conservatively taint counters whenever counting may have occurred;
        // disabling counting does not recover a value that was never measured.
        auto unavailable=memory->metrics.unavailable_counters;
        const auto active=[](std::uint32_t control) -> std::uint8_t {
            std::uint8_t mask=0;
            for (unsigned index=0;index<2;++index) {
                // Selector 16 explicitly disables an individual counter. The
                // primary counter also has an unused zero selector. Reserved
                // selectors remain unknown rather than acquiring semantics.
                if (performance_active(control,index)) mask|=1U<<index;
            }
            return mask;
        };
        unavailable|=active(candidate.control);
        result.status=Stop::budget;
        switch (request) {
        case PerformanceOperation::read_clock: result.status=Stop::unsupported_system; break;
        case PerformanceOperation::read_control: result.value=candidate.control; break;
        case PerformanceOperation::write_control:
            candidate.control=value; unavailable|=active(value); break;
        case PerformanceOperation::read_counter:
            if (unavailable&(1U<<counter)) result.status=Stop::unsupported_system;
            else result.value=candidate.counters[counter];
            break;
        case PerformanceOperation::write_counter:
            candidate.counters[counter]=value;
            unavailable&=static_cast<std::uint8_t>(~(1U<<counter));
            unavailable|=active(candidate.control);
            break;
        }
        if (result.status==Stop::budget) memory->metrics.unavailable_counters=unavailable;
    }
    memory->metrics.rejected=result.status!=Stop::budget;
    memory->metrics.rejected_operation=static_cast<std::uint8_t>(operation);
    memory->metrics.rejected_counter=static_cast<std::uint8_t>(counter);
    memory->metrics.rejected_control=request==PerformanceOperation::write_control ? value : memory->performance.control;
    if (result.status==Stop::budget) memory->performance=candidate;
    return (std::uint64_t(result.status)<<32)|result.value;
}
namespace {
void publish(nt::ee::Memory& memory,nt::ee::Stop status,std::uint32_t address) {
    if (memory.io.poll) memory.io.poll(memory.io.context);
    if (memory.interrupts) memory.interrupts->publish(memory);
    if (memory.completions) memory.completions->publish(memory);
    // A native producer rejecting a code write supplies its actual destination,
    // which is more useful than the service endpoint that submitted the copy.
    if (status!=nt::ee::Stop::budget && status!=nt::ee::Stop::code_write) memory.fault_address=address;
}
}
extern "C" std::uint64_t nt_ee_io_read32(nt::ee::Memory* memory,std::uint32_t address) {
    const auto result=memory->io.read32 ? memory->io.read32(memory->io.context,address) :
        nt::ee::IoRead{nt::ee::Stop::memory_fault};
    publish(*memory,result.status,address);
    return (std::uint64_t(result.status)<<32)|result.value;
}
extern "C" std::uint64_t nt_ee_test_completions(nt::ee::Memory* memory) {
    // Drain actual host completion notifications before observing the selection.
    publish(*memory,nt::ee::Stop::budget,0);
    const auto result=memory->io.test_completions ? memory->io.test_completions(memory->io.context) :
        nt::ee::IoRead{nt::ee::Stop::unsupported_system};
    return (std::uint64_t(result.status)<<32)|result.value;
}
extern "C" std::uint32_t nt_ee_io_write8(nt::ee::Memory* memory,std::uint32_t address,std::uint32_t value) {
    const auto result=memory->io.write8 ? memory->io.write8(memory->io.context,address,static_cast<std::uint8_t>(value)) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}
extern "C" std::uint32_t nt_ee_io_write32(nt::ee::Memory* memory,std::uint32_t address,std::uint32_t value) {
    const auto result=memory->io.write32 ? memory->io.write32(memory->io.context,address,value) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint32_t nt_ee_io_read64(nt::runtime::State* state,nt::ee::Memory* memory,std::uint32_t address,std::uint32_t reg) {
    const auto result=memory->io.read64 ? memory->io.read64(memory->io.context,address) : nt::ee::IoRead64{nt::ee::Stop::memory_fault};
    publish(*memory,result.status,address);
    if (result.status==nt::ee::Stop::budget && reg && reg<32) state->set(reg,result.value);
    return static_cast<std::uint32_t>(result.status);
}
extern "C" std::uint32_t nt_ee_io_write64(nt::ee::Memory* memory,std::uint32_t address,std::uint64_t value) {
    const auto result=memory->io.write64 ? memory->io.write64(memory->io.context,address,value) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}
extern "C" std::uint32_t nt_ee_io_write128(nt::ee::Memory* memory,std::uint32_t address,std::uint64_t low,std::uint64_t high) {
    const auto result=memory->io.write128 ? memory->io.write128(memory->io.context,address,{low,high}) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint64_t nt_ee_address_space(nt::ee::Memory* memory,std::uint32_t operation,std::uint32_t parameter,std::uint32_t value) {
    using nt::ee::AddressSpaceOperation;
    if (operation>static_cast<std::uint32_t>(AddressSpaceOperation::probe))
        return std::uint64_t(nt::ee::Stop::invalid_state)<<32;
    const auto result=memory->address_space.request ?
        memory->address_space.request(memory->address_space.context,static_cast<AddressSpaceOperation>(operation),parameter,value) :
        nt::ee::IoRead{nt::ee::Stop::unsupported_system};
    return (std::uint64_t(result.status)<<32)|result.value;
}


// This consumes a converted layout declaration, not a sequence of CPU register
// writes. It owns no simulated entries and cannot redirect individual accesses.
// Unsupported aliases require a converter/native allocation implementation.
extern "C" std::uint32_t nt_ee_validate_native_layout(nt::ee::Memory* memory,
    std::uint32_t slot,std::uint32_t mask,std::uint32_t address,std::uint32_t first,std::uint32_t second,std::uint32_t planned) {
    using nt::ee::Stop;
    const auto unsupported=static_cast<std::uint32_t>(Stop::unsupported_system);
    if (slot>=48 || (address&255)) return unsupported;
    if (first&0x80000000U) {
        if (mask || address!=nt::ee::scratchpad_begin || (first&6U)!=6U || (first&0x7fffffc0U) || (second&0xffffffc0U) ||
            !memory->scratchpad || memory->scratchpad_bytes<nt::ee::scratchpad_size) return unsupported;
        return static_cast<std::uint32_t>(Stop::budget);
    }
    bool valid_mask=false;
    for (const auto candidate:{0U,0x6000U,0x1e000U,0x7e000U,0x1fe000U,0x7fe000U,0x1ffe000U})
        valid_mask|=mask==candidate;
    if (!valid_mask || (address&(mask|0x1fffU))) return unsupported;
    if (planned && !first && !second) return static_cast<std::uint32_t>(Stop::budget);
    const std::uint32_t page=(mask+0x2000U)/2,entries[]{first,second};
    for (unsigned half=0;half<2;++half) {
        const auto entry=entries[half];
        if (entry&0xfc000000U) return unsupported;
        const auto physical=(entry&0x03ffffc0U)<<6;
        if (physical%page) return unsupported;
        if (planned && physical>=0x10000000U && std::uint64_t(physical)+page<=0x20000000U &&
            std::uint64_t(address)+std::uint64_t(half)*page==physical) {
            if (!memory->io.read32 || !memory->io.write32) return unsupported;
            continue; // Existing native device interfaces; generated guards preserve denied accesses.
        }
        if ((entry&6)!=6 || !memory->data || std::uint64_t(physical)+page>memory->size) return unsupported;
        if (planned) continue; // Caller emitted an exact match to a validated static layout.
        for (std::uint64_t offset=0;offset<page;offset+=4096) {
            const auto logical=std::uint64_t(address)+std::uint64_t(half)*page+offset;
            if (logical>0xffffffffULL || nt::runtime::default_ram_address(static_cast<std::uint32_t>(logical))!=physical+offset)
                return unsupported;
        }
    }
    return static_cast<std::uint32_t>(Stop::budget);
}
