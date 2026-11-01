#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>

namespace nt::iop {
// The public IOP jump buffer stores twelve 32-bit register values. Restoring
// one transfers to already compiled code; it never decodes a saved instruction.
class NativeJumpServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sysclib || (request.selector!=4 && request.selector!=5)) return {};
        if (!request.context || request.interrupt_context) return {NativeServiceStatus::unavailable};
        const auto offset=ram_offset(request.arguments[0],4,ram.size());
        if (!offset || 48>ram.size()-*offset || 48>0x200000-*offset)
            return {NativeServiceStatus::address_fault,0,request.arguments[0]};
        constexpr std::array<unsigned,12> registers{31,29,30,16,17,18,19,20,21,22,23,28};
        auto state=*request.context;
        if (state.load_register) state.registers[state.load_register]=state.load_value;
        state.load_register=0;
        if (request.selector==4) {
            for (unsigned word=0;word<registers.size();++word)
                if (nt_iop_native_code_word(*offset+word*4)) return {NativeServiceStatus::code_write,0,*offset+word*4};
            for (unsigned word=0;word<registers.size();++word)
                store_native_word(ram,*offset+word*4,state.registers[registers[word]]);
            return {NativeServiceStatus::returned,0};
        }
        // A nonlocal transfer cannot abandon an active native service's owned
        // callback/cleanup continuation. Such unwinding is explicitly unsupported.
        if (state.native_call_owner) return {NativeServiceStatus::unavailable};
        const auto target=load_native_word(ram,*offset);
        const auto physical=ram_offset(target,4,ram.size());
        if (!physical || !nt_iop_native_instruction(*physical)) return {NativeServiceStatus::unmapped_code,0,target};
        for (unsigned word=0;word<registers.size();++word)
            state.registers[registers[word]]=load_native_word(ram,*offset+word*4);
        state.registers[0]=0; state.registers[2]=request.arguments[1] ? request.arguments[1] : 1;
        state.pc=target; state.delay_slot=false;
        NativeServiceReply reply{NativeServiceStatus::jumped}; reply.restore=state; return reply;
    }
};
}
