#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/gamepad.hpp>
#include <nt/binary.hpp>
#include <array>
#include <cstring>
#include <functional>

namespace nt::ee {
// Public legacy libpad endpoint backed by native host input. No ROM, instruction
// execution, serial bus model or fabricated device connection is involved.
class NativePadRpc final : public NativeRpcService {
public:
    using Sampler=std::function<input::PadSnapshot(unsigned)>;
    explicit NativePadRpc(Sampler sampler)
        : NativeRpcService(0x8000010fU,0xa000,0xb000),sample_(std::move(sampler)) {}
    Stop update(Memory& memory,CodeOverlap protection) {
        // Validate every buffer before publishing a multi-port snapshot.
        for (const auto& port:ports_) if (port.address) {
            const auto valid=rpc_output(memory,protection,port.address,128);
            if (valid!=Stop::budget) return valid;
        }
        for (unsigned i=0;i<ports_.size();++i) if (ports_[i].address) publish(memory,i);
        return Stop::budget;
    }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        if (method!=1) return Stop::unsupported_system;
        if (!sample_ || args.size()!=128 || result_size!=128) return Stop::invalid_state;
        auto valid=rpc_output(memory,protection,destination,result_size);
        if (valid!=Stop::budget) return valid;
        const auto command=le32(args,0),index=le32(args,4),slot=le32(args,8);
        if (index>=ports_.size() || slot!=0) return Stop::unsupported_system;
        auto& port=ports_[index];
        if (port.address) {
            valid=rpc_output(memory,protection,port.address,128);
            if (valid!=Stop::budget) return valid;
        }
        std::array<std::uint8_t,128> response{};
        auto put=[&](unsigned offset,std::uint32_t value) { std::memcpy(response.data()+offset,&value,4); };
        const auto snapshot=sample_(index);
        switch (command) {
        case 0x80000100: {
            const auto address=runtime::default_ram_address(le32(args,16));
            if (!address || address%64 || port.address) return Stop::invalid_state;
            valid=rpc_output(memory,protection,address,128);
            if (valid!=Stop::budget) return valid;
            const auto out=runtime::default_ram_address(destination);
            if (out<std::uint64_t(address)+128 && address<std::uint64_t(out)+128) return Stop::invalid_state;
            for (const auto& other:ports_) if (other.address && address<std::uint64_t(other.address)+128 && other.address<std::uint64_t(address)+128)
                return Stop::invalid_state;
            port.address=address;
            port.analog=false;
            publish(memory,index);
            put(12,1); // Port subscription exists even when the physical device is absent.
            break;
        }
        case 0x80000104: { // mode query
            if (!port.address) return Stop::invalid_state;
            const auto info=le32(args,12),entry=le32(args,16);
            std::uint32_t result=0;
            if (info<1 || info>4) return Stop::unsupported_system;
            if (snapshot.connected) {
                if (info==1) result=port.analog ? 7 : 4;
                if (info==2) result=7;
                if (info==3) result=port.analog ? 1 : 0;
                if (info==4) result=entry==0xffffffff ? 2 : entry==0 ? 4 : entry==1 ? 7 : 0;
            }
            put(20,result); break;
        }
        case 0x80000105: { // mode switch, completed by the next native publication
            if (!port.address) return Stop::invalid_state;
            const auto mode=le32(args,12),lock=le32(args,16);
            if (mode>1 || (lock!=0 && lock!=3)) return Stop::unsupported_system;
            if (snapshot.connected) { port.analog=mode!=0; put(20,1); }
            break;
        }
        case 0x80000108: // Standard buttons only: no pressure sensor capability.
            if (!port.address) return Stop::invalid_state;
            put(12,snapshot.connected ? 0xffff : 0); break;
        case 0x80000102: // No actuator interface is exposed by this provider.
            if (!port.address) return Stop::invalid_state;
            put(20,0); break;
        case 0x8000010d:
            if (!port.address) return Stop::invalid_state;
            port={}; put(12,1); break;
        default: return Stop::unsupported_system;
        }
        std::memcpy(memory.data+runtime::default_ram_address(destination),response.data(),response.size());
        return Stop::budget;
    }
    // The old library binds a second endpoint; this adapter has no implemented
    // operations on that endpoint. Binding must not imply successful calls.
    class Auxiliary final : public NativeRpcService {
    public:
        Auxiliary() : NativeRpcService(0x8000011fU,0xc000,0xd000) {}
        Stop call(Memory&,CodeOverlap,std::uint32_t,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t) override {
            return Stop::unsupported_system;
        }
    };
private:
    struct Port { std::uint32_t address=0,frame=0; bool analog=false; };
    std::array<Port,2> ports_{};
    Sampler sample_;
    void publish(Memory& memory,unsigned index) {
        auto& port=ports_[index];
        const auto sample=sample_(index);
        std::array<std::uint8_t,64> record{};
        // Both records contain the same completed snapshot. Avoid wrap-sensitive
        // selection between buffers in legacy clients.
        ++port.frame;
        std::memcpy(record.data(),&port.frame,4);
        record[4]=sample.connected ? 6 : 0;
        record[5]=0; // request complete; the native owner applied mode changes
        record[8]=sample.connected ? 0 : 0xff;
        record[9]=port.analog ? 0x73 : 0x41;
        const auto buttons=static_cast<std::uint16_t>(~sample.buttons);
        record[10]=std::uint8_t(buttons); record[11]=std::uint8_t(buttons>>8);
        for (unsigned i=0;i<4;++i) record[12+i]=sample.axes[i];
        const std::uint32_t length=sample.connected ? (port.analog ? 8 : 4) : 0;
        std::memcpy(record.data()+40,&length,4);
        for (unsigned offset:{0U,64U}) std::memcpy(memory.data+port.address+offset,record.data(),record.size());
    }
};
}
