#pragma once
#include <nt/ee_native.hpp>

namespace nt::ee {
enum class ExceptionCode : std::uint32_t {
    interrupt=0, load_address=4, store_address=5, data_bus=7,
    syscall=8, breakpoint=9, overflow=12, trap=13
};
// Capture a native boundary without entering a BIOS vector or changing the
// service scheduler's exception level. Existing handler entry owns delivery.
inline void record_exception(runtime::State& state,Memory& memory,ExceptionCode code,
                             std::uint32_t address=0) {
    auto& record=memory.exception;
    auto delay=record.cause&0x80000000U;
    if (!(state.cop0_status&2U)) {
        delay=state.delay_slot ? 0x80000000U : 0;
        state.cop0_epc=state.delay_slot ? state.branch_pc : state.pc;
    }
    record.cause=delay|(static_cast<std::uint32_t>(code)<<2);
    if (code==ExceptionCode::load_address || code==ExceptionCode::store_address) {
        record.virtual_address=address; record.virtual_valid=true;
    } else if (code==ExceptionCode::data_bus) {
        record.physical_address=address; record.physical_valid=true;
    }
}
inline IoRead read_exception(Memory& memory,std::uint32_t selector) {
    auto& record=memory.exception;
    record.unavailable_selector=0;
    if (selector==13) return {Stop::budget,record.cause|(memory.pending_interrupts&0xff00U)};
    if (selector==8 && record.virtual_valid) return {Stop::budget,record.virtual_address};
    if (selector==23 && record.physical_valid) return {Stop::budget,record.physical_address};
    record.unavailable_selector=selector;
    return {Stop::unsupported_system};
}
}
