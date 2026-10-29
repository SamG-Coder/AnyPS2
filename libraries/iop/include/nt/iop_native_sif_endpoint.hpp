#pragma once
#include <nt/iop_native_sif.hpp>

namespace nt::iop {
// Explicit converted-peer endpoint: data is copied before a command becomes
// available. The peer consumes an owned packet, not an emulated DMA interrupt.
class NativeSifCommandEndpoint {
public:
    NativeSifCommandEndpoint(NativeSifDestination& destination,std::uint32_t command_buffer,NativeSifTransfers& completions)
        : destination_(destination), completions_(completions), command_buffer_(command_buffer) {
        if (destination.memory.size()>0x2000000 || !destination.writable)
            throw std::invalid_argument("Invalid native SIF command destination");
    }
    bool accepting() const { return completions_.accepting(); }
    NativeServiceReply snapshot(std::uint32_t address,std::uint32_t size,std::vector<std::uint8_t>& result) const {
        if (!accepting() || size>0x100000) return {};
        const auto offset=address&0x1fffffffU;
        if (size && (offset>destination_.memory.size() || size>destination_.memory.size()-offset))
            return {NativeServiceStatus::address_fault,0,address};
        std::vector<std::uint8_t> bytes;
        if (size) bytes.assign(destination_.memory.begin()+offset,destination_.memory.begin()+offset+size);
        result.swap(bytes);
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply submit(std::span<const std::uint8_t> packet,std::span<const std::uint8_t> extra,std::optional<NativeCallback> callback={}) {
        if (!completions_.accepting()) return {};
        if (packet.size()<16 || packet.size()>112 || packet[0]!=packet.size() ||
            (load_native_word(packet,0)>>8)!=extra.size() || extra.size()>0x100000) return {};
        if (pending_) return {NativeServiceStatus::returned};
        const auto extra_address=load_native_word(packet,4);
        auto valid=validate(command_buffer_,packet.size());
        if (valid.status!=NativeServiceStatus::returned) return valid;
        if (!extra.empty()) {
            valid=validate(extra_address,extra.size());
            if (valid.status!=NativeServiceStatus::returned) return valid;
        }
        // Allocate and snapshot before committing either write, including aliases.
        std::vector<std::uint8_t> command(packet.begin(),packet.end()),payload(extra.begin(),extra.end());
        return completions_.complete([&]() noexcept {
            if (!payload.empty()) std::copy(payload.begin(),payload.end(),destination_.memory.begin()+(extra_address&0x1fffffffU));
            std::copy(command.begin(),command.end(),destination_.memory.begin()+(command_buffer_&0x1fffffffU));
            pending_=std::move(command);
        },callback);
    }
    // Publish data before its completion record without occupying the command
    // mailbox. Validate and snapshot both regions before either becomes visible.
    NativeServiceReply publish_result(std::uint32_t record_address,std::span<const std::uint8_t> record,
                                      std::uint32_t data_address,std::span<const std::uint8_t> data) {
        if (!accepting() || record.empty() || record.size()>112 || data.size()>0x100000) return {};
        auto checked=validate(record_address,record.size());
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (!data.empty()) {
            checked=validate(data_address,data.size());
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        std::vector<std::uint8_t> completion(record.begin(),record.end()),payload(data.begin(),data.end());
        return completions_.complete([&]() noexcept {
            if (!payload.empty()) std::copy(payload.begin(),payload.end(),destination_.memory.begin()+(data_address&0x1fffffffU));
            std::copy(completion.begin(),completion.end(),destination_.memory.begin()+(record_address&0x1fffffffU));
        });
    }
    // Validate an address publication before signalling dependent waiters. The
    // supplied operation must return without changes when it cannot commit.
    template<class BeforePublish>
    NativeServiceReply publish_buffer(std::uint32_t address,BeforePublish&& before_publish) {
        if (!completions_.accepting()) return {};
        const auto offset=address&0x1fffffffU;
        if ((address&15) || offset>destination_.memory.size() || destination_.memory.size()-offset<16)
            return {NativeServiceStatus::address_fault,0,address};
        if (!destination_.writable(offset,16)) return {NativeServiceStatus::code_write,0,address};
        const auto result=before_publish();
        if (result.status==NativeServiceStatus::returned && result.value==0) command_buffer_=address;
        return result;
    }
    std::optional<std::vector<std::uint8_t>> take() {
        auto packet=std::move(pending_); pending_.reset(); return packet;
    }
private:
    NativeServiceReply validate(std::uint32_t address,std::size_t size) const {
        const auto offset=address&0x1fffffffU;
        if (offset>destination_.memory.size() || size>destination_.memory.size()-offset)
            return {NativeServiceStatus::address_fault,0,address};
        if (!destination_.writable(offset,static_cast<std::uint32_t>(size)))
            return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    NativeSifDestination& destination_;
    NativeSifTransfers& completions_;
    std::uint32_t command_buffer_;
    std::optional<std::vector<std::uint8_t>> pending_;
};
}
