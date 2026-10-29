#pragma once
#include <nt/iop_native.hpp>

namespace nt::iop {
enum class NativeServiceKind { syscall, sysmem, loadcore, library_call, ioman, intrman, thevent, thbase, sifman, stdio, sifcmd, thsemap, thmsgbx, native_return };
struct NativeServiceRequest {
    std::uint32_t selector;
    std::array<std::uint32_t,4> arguments;
    NativeServiceKind kind=NativeServiceKind::syscall;
    std::uint32_t call_site=0;
    bool interrupt_context=false;
    std::uint32_t global_pointer=0;
    std::uint32_t stack_pointer=0;
    const Cpu* context=nullptr; // Stable native continuation identity; never exposed to guest RAM.
    std::uint32_t resume_pc=0;
};
// unsupported declines ownership; unavailable claims the request but stops execution.
enum class NativeServiceStatus { unsupported, returned, address_fault, transferred, code_write, unmapped_code, yielded, called, restored, waiting, retry_yielded, unavailable };
struct NativeServiceCall {
    // Ownership prevents nested providers from consuming each other's returns.
    std::array<std::uint32_t,4> arguments{};
    std::uint32_t global_pointer=0;
    const void* owner=nullptr;
};
inline constexpr std::uint32_t native_service_return=0xfffffff0U;
struct NativeServiceReply {
    NativeServiceStatus status=NativeServiceStatus::unsupported;
    std::uint32_t value=0, address=0;
    std::optional<NativeServiceCall> call;
    std::optional<Cpu> restore;
    std::optional<std::uint32_t> high_return;
};
class NativeServices {
public:
    virtual ~NativeServices()=default;
    // Implementations must validate before changing RAM. Unsupported requests
    // must have no side effects. No services are registered implicitly.
    virtual NativeServiceReply invoke(const NativeServiceRequest&,std::span<std::uint8_t>)=0;
};

// Drive compiled code across explicitly supplied native return-value services.
// Cpu must retain its address while a service-owned call continuation is pending.
// This loop never reads instructions. Kernel context switches and delay-slot services
// require a different contract and remain explicit unsupported boundaries.
inline NativeResult run_native(NativeEntry entry,Cpu& cpu,std::span<std::uint8_t> ram,
                               std::uint64_t budget,NativeServices* services=nullptr) {
    while (true) {
        const auto before=cpu.executed;
        auto result=entry(cpu,ram,budget);
        budget-=cpu.executed-before;
        const bool imported=result.reason==NativeStop::import_request;
        const bool returning=result.reason==NativeStop::unmapped_code && cpu.pc==native_service_return;
        if ((!imported && !returning && result.reason!=NativeStop::service_request) || !services || cpu.delay_slot) return result;
        if (!budget) return {NativeStop::budget,cpu.pc};
        const auto reg=[&](unsigned index) {
            return (imported || returning) && cpu.load_register==index ? cpu.load_value : cpu.registers[index];
        };
        auto kind=returning ? NativeServiceKind::native_return : NativeServiceKind::syscall;
        if (imported) switch (result.trap_code>>16) {
        case 0: kind=NativeServiceKind::sysmem; break;
        case 1: kind=NativeServiceKind::loadcore; break;
        case 2: kind=NativeServiceKind::library_call; break;
        case 3: kind=NativeServiceKind::ioman; break;
        case 4: kind=NativeServiceKind::intrman; break;
        case 5: kind=NativeServiceKind::thevent; break;
        case 6: kind=NativeServiceKind::thbase; break;
        case 7: kind=NativeServiceKind::sifman; break;
        case 8: kind=NativeServiceKind::stdio; break;
        case 9: kind=NativeServiceKind::sifcmd; break;
        case 10: kind=NativeServiceKind::thsemap; break;
        case 11: kind=NativeServiceKind::thmsgbx; break;
        default: return result;
        }
        const auto resume=imported ? reg(31) : cpu.pc+4;
        const NativeServiceRequest request{imported ? result.trap_code&65535U : cpu.registers[2],
            returning ? std::array<std::uint32_t,4>{reg(2),reg(3),0,0} : std::array<std::uint32_t,4>{reg(4),reg(5),reg(6),reg(7)},
            kind,cpu.pc,cpu.interrupt_depth!=0,reg(28),reg(29),&cpu,resume};
        const auto reply=services->invoke(request,ram);
        if (reply.status==NativeServiceStatus::unsupported || reply.status==NativeServiceStatus::unavailable) return result;
        if (reply.status==NativeServiceStatus::address_fault) return {NativeStop::address_fault,cpu.pc,reply.address};
        if (reply.status==NativeServiceStatus::code_write) return {NativeStop::code_write,cpu.pc,reply.address};
        if (reply.status==NativeServiceStatus::unmapped_code) return {NativeStop::unmapped_code,cpu.pc,reply.address};
        if (reply.status==NativeServiceStatus::retry_yielded) return {NativeStop::thread_yield,cpu.pc};
        if (reply.status==NativeServiceStatus::waiting) return {NativeStop::thread_wait,cpu.pc};
        if (reply.status==NativeServiceStatus::restored) {
            if (!returning || !reply.restore) return result;
            const auto executed=cpu.executed; cpu=*reply.restore; cpu.executed=executed;
            continue;
        }
        if (reply.status==NativeServiceStatus::called) {
            if (returning || !reply.call) return result;
            if (cpu.load_register) cpu.registers[cpu.load_register]=cpu.load_value;
            cpu.load_register=0;
            for (unsigned index=0;index<4;++index) cpu.registers[4+index]=reply.call->arguments[index];
            cpu.native_call_owner=reply.call->owner;
            cpu.registers[28]=reply.call->global_pointer; cpu.registers[31]=native_service_return;
            cpu.registers[0]=0; cpu.pc=reply.value; ++cpu.executed; --budget;
            if (!budget) return {NativeStop::budget,cpu.pc};
            continue;
        }
        // A returned service consumes one native operation from the caller's slice.
        // Its result replaces v0; other pending loads commit on retirement.
        const bool transfer=reply.status==NativeServiceStatus::transferred;
        if (cpu.load_register && (cpu.load_register!=2 || transfer)) cpu.registers[cpu.load_register]=cpu.load_value;
        cpu.load_register=0; if (!transfer) cpu.registers[2]=reply.value; cpu.registers[0]=0;
        if (!transfer && reply.high_return) cpu.registers[3]=*reply.high_return;
        cpu.pc=transfer ? reply.value : resume; ++cpu.executed; --budget;
        if (reply.status==NativeServiceStatus::yielded) return {NativeStop::thread_yield,cpu.pc};
        if (!budget) return {NativeStop::budget,cpu.pc};
    }
}
}
