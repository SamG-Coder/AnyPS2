#pragma once
#include <nt/iop_native_format.hpp>
#include <nt/iop_native_code.hpp>

namespace nt::iop {
class NativePrntServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::native_return) {
            if (!pending_ || !request.context || request.context!=pending_->owner || request.context->native_call_owner!=this) return {};
            ++pending_->position;
            if (pending_->position<pending_->output.size()+2) return next();
            auto caller=pending_->caller; caller.registers[2]=static_cast<std::uint32_t>(pending_->output.size());
            pending_.reset();
            NativeServiceReply reply{NativeServiceStatus::restored}; reply.restore=caller; return reply;
        }
        if (request.kind!=NativeServiceKind::sysclib || request.selector!=18) return {};
        if (pending_ || !request.context || request.interrupt_context) return {NativeServiceStatus::unavailable};
        const auto target=ram_offset(request.arguments[0],4,0x200000);
        if (!request.arguments[0] || !target || !nt_iop_native_instruction(*target))
            return {NativeServiceStatus::unmapped_code,0,request.arguments[0]};
        unsigned argument=0;
        const auto read=[&](std::uint32_t& value,NativeServiceReply& failure) {
            if (argument>=128) return false;
            const auto address=std::uint64_t(request.arguments[3])+argument*4;
            const auto offset=address<=0xffffffffU ? ram_offset(static_cast<std::uint32_t>(address),4,ram.size()) : std::nullopt;
            if (!offset || ram.size()-*offset<4 || 0x200000-*offset<4) {
                failure={NativeServiceStatus::address_fault,0,request.arguments[3]}; return false;
            }
            value=load_native_word(ram,*offset); ++argument; return true;
        };
        std::string output;
        auto formatted=format_native(request.arguments[2],ram,read,output);
        if (formatted.status!=NativeServiceStatus::returned) {
            if (formatted.status==NativeServiceStatus::unsupported) formatted.status=NativeServiceStatus::unavailable;
            return formatted;
        }
        Cpu caller=*request.context;
        if (caller.load_register) caller.registers[caller.load_register]=caller.load_value;
        caller.load_register=0; caller.delay_slot=false; caller.pc=request.resume_pc; caller.registers[0]=0;
        pending_=Pending{request.context,caller,request.arguments[0],request.arguments[1],request.global_pointer,std::move(output),0};
        return next();
    }
private:
    struct Pending {
        const Cpu* owner; Cpu caller; std::uint32_t target,context,gp;
        std::string output; std::size_t position;
    };
    NativeServiceReply next() const {
        const auto& call=*pending_;
        const auto character=call.position<call.output.size() ? static_cast<unsigned char>(call.output[call.position]) :
            call.position==call.output.size() ? 513U : 512U;
        NativeServiceReply reply{NativeServiceStatus::called,call.target};
        reply.call=NativeServiceCall{{call.context,character,0,0},call.gp,this}; return reply;
    }
    std::optional<Pending> pending_;
};
}
