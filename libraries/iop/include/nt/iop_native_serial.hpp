#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_interrupts.hpp>
#include <nt/native_port_exchange.hpp>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace nt::iop {
// Session-owned transaction adapter for compiled peripheral drivers. Native
// endpoints acknowledge completed requests; unbound ports remain disconnected.
class NativeSerialServices final : public NativeServices {
public:
    void connect_disconnected_ports(NativeInterruptServices& events) { events_=&events; }
    void bind_ports(NativePortHandler handler) { handler_=std::move(handler); }
    void reset() {
        commands_={}; ports_={}; control_=0; interrupts_=0; output_.clear(); input_.clear();
        cursor_=0; status_=0; completed_=false;
        failure_=0; expected_output_=0; expected_input_=0; buffers_={}; dma_output_.clear();
        if (events_) events_->clear_pending(17);
    }
    std::span<const std::uint8_t> pending_output() const { return output_; }
    unsigned failure() const { return failure_; }
    unsigned expected_output() const { return expected_output_; }
    unsigned expected_input() const { return expected_input_; }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::dmacman) return buffer_request(request,ram);
        if (request.kind!=NativeServiceKind::serial) return {};
        const auto address=request.arguments[0], value=request.arguments[1];
        if (request.arguments[2]==1) {
            if (request.selector==0 && address==0x1f808264) {
                if (!completed_ || cursor_==input_.size()) return {NativeServiceStatus::unavailable};
                return {NativeServiceStatus::returned,input_[cursor_++]};
            }
            if (request.selector!=1 || address!=0x1f808260) return {NativeServiceStatus::unavailable};
            std::size_t expected=0;
            for (const auto command:commands_) {
                const auto length=(command>>8)&1023U;
                if (!length) break;
                expected+=length;
            }
            if (output_.size()>=expected || output_.size()>=4096) return {NativeServiceStatus::unavailable};
            output_.push_back(static_cast<std::uint8_t>(value));
            return {NativeServiceStatus::returned};
        }
        if (request.arguments[2]!=0 && request.arguments[2]!=4) return {NativeServiceStatus::unavailable};
        if ((address&3U) || request.selector>1) return {NativeServiceStatus::unavailable};
        const bool write=request.selector==1;
        if (address>=0x1f808200 && address<0x1f808260) {
            const auto offset=(address-0x1f808200)/4;
            auto& setting=offset<16 ? commands_[offset] : ports_[offset-16];
            if (write) {
                setting=value;
                if (!offset) {
                    output_.clear(); input_.clear(); cursor_=0; completed_=false; status_=0;
                    buffers_={}; dma_output_.clear();
                }
            }
            return {NativeServiceStatus::returned,setting};
        }
        if (address==0x1f808268) {
            if (write) {
                if (value&1U) {
                    failure_=0;
                    if (!events_ || completed_ || interrupts_) {
                        failure_=!events_ ? 1 : completed_ ? 2 : 3;
                        return {NativeServiceStatus::unavailable};
                    }
                    std::size_t sent=0,received=0;
                    unsigned ports=0,command_count=0;
                    bool block_lengths_valid=true;
                    for (const auto command:commands_) {
                        const auto length=(command>>8)&1023U;
                        if (!length) break;
                        sent+=length; received+=(command>>18)&1023U; ports|=1U<<(command&1);
                        ++command_count;
                        if (!dma_output_.empty() && length>buffers_[0].block_bytes) block_lengths_valid=false;
                        if (buffers_[1].armed && ((command>>18)&1023U)>buffers_[1].block_bytes) block_lengths_valid=false;
                    }
                    expected_output_=static_cast<unsigned>(sent); expected_input_=static_cast<unsigned>(received);
                    const bool buffered=!dma_output_.empty();
                    if (!sent || !received || received>16384 || !block_lengths_valid ||
                        (buffered ? (!output_.empty() || command_count!=buffers_[0].blocks) : sent!=output_.size()) ||
                        (buffers_[1].armed && command_count!=buffers_[1].blocks)) {
                        failure_=4;
                        return {NativeServiceStatus::unavailable};
                    }
                    if (buffers_[1].armed) {
                        const auto& target=buffers_[1];
                        const auto extent=target.block_bytes*target.blocks;
                        if (target.offset>ram.size() || extent>ram.size()-target.offset)
                            return {NativeServiceStatus::address_fault,0,target.offset};
                    }
                    std::vector<std::uint8_t> response(received,0xff);
                    std::vector<NativePortExchange> exchanges;
                    const auto source=std::span<const std::uint8_t>(buffered ? dma_output_ : output_);
                    std::size_t source_offset=0,response_offset=0;
                    for (unsigned block=0;block<command_count;++block) {
                        const auto command=commands_[block];
                        const unsigned length=(command>>8)&1023U, size=(command>>18)&1023U;
                        exchanges.push_back({command&1U,source.subspan(source_offset,length),
                            std::span(response).subspan(response_offset,size)});
                        source_offset+=buffered ? buffers_[0].block_bytes : length;
                        response_offset+=size;
                    }
                    if (handler_ && !handler_(exchanges)) {
                        failure_=5;
                        return {NativeServiceStatus::unavailable};
                    }
                    unsigned missing=0;
                    for (const auto& exchange:exchanges)
                        if (!exchange.acknowledged) missing|=1U<<exchange.port;
                    input_=std::move(response); cursor_=0; completed_=true;
                    if (buffers_[1].armed) {
                        auto& target=buffers_[1];
                        // All bounds/code protection were checked while arming.
                        // Publish each device response, then the transport's block padding.
                        std::size_t offset=0;
                        for (unsigned block=0;block<command_count;++block) {
                            const unsigned response=(commands_[block]>>18)&1023U;
                            auto destination=ram.subspan(target.offset+block*target.block_bytes,target.block_bytes);
                            std::fill(destination.begin(),destination.end(),0);
                            std::copy_n(input_.begin()+offset,response,destination.begin());
                            offset+=response;
                        }
                        cursor_=input_.size(); target.armed=false; events_->post(45);
                    }
                    status_=(missing ? 0xd000U : 0x1000U)|(missing<<16)|(ports==3 ? 0x200U : 0x100U);
                    interrupts_=1; control_=value&~1U; events_->post(17);
                    return {NativeServiceStatus::returned,control_};
                }
                if (value&0xcU) { interrupts_=0; if (events_) events_->clear_pending(17); }
                control_=value;
            }
            return {NativeServiceStatus::returned,control_};
        }
        if (address==0x1f808280) {
            if (write) { interrupts_&=~value; if (!interrupts_ && events_) events_->clear_pending(17); }
            return {NativeServiceStatus::returned,interrupts_};
        }
        // Port status is readable before a transaction completes. This adapter
        // reading it must not fabricate FIFO bytes or a completion interrupt.
        if (!write && events_ && address==0x1f808270) return {NativeServiceStatus::returned,0xf};
        if (!write && completed_) {
            if (address==0x1f80826c) return {NativeServiceStatus::returned,status_};
            if (address==0x1f808274) return {NativeServiceStatus::returned,0}; // No device-specific response flags.
        }
        return {NativeServiceStatus::unavailable};
    }
private:
    struct Buffer { unsigned offset=0,block_bytes=0,blocks=0; bool configured=false,armed=false; };
    NativeServiceReply buffer_request(const NativeServiceRequest& r,std::span<std::uint8_t> ram) {
        if ((r.arguments[0]!=11 && r.arguments[0]!=12) || (r.selector!=28 && r.selector!=32)) return {};
        if (!events_) return {NativeServiceStatus::unavailable};
        auto& buffer=buffers_[r.arguments[0]-11];
        if (r.selector==28) {
            const auto fifth=ram_offset(r.stack_pointer+16,4,ram.size());
            if (r.stack_pointer>0xffffffe0U || !fifth) return {NativeServiceStatus::address_fault,0,r.stack_pointer};
            const auto direction=load_native_word(ram,*fifth);
            const std::uint64_t size=std::uint64_t(r.arguments[2])*r.arguments[3]*4;
            if (direction!=(r.arguments[0]==11 ? 1U : 0U) || !size || size>65536 ||
                !r.arguments[2] || r.arguments[3]>16 || buffer.armed) return {NativeServiceStatus::unavailable};
            const auto offset=ram_offset(r.arguments[1],4,ram.size());
            if (!offset || size>ram.size()-*offset) return {NativeServiceStatus::address_fault,0,r.arguments[1]};
            buffer={*offset,r.arguments[2]*4,r.arguments[3],true,false};
            return {NativeServiceStatus::returned,1};
        }
        if (!buffer.configured || buffer.armed) return {NativeServiceStatus::unavailable};
        const auto size=buffer.block_bytes*buffer.blocks;
        if (buffer.offset>ram.size() || size>ram.size()-buffer.offset) return {NativeServiceStatus::address_fault};
        if (r.arguments[0]==11) {
            if (!dma_output_.empty()) return {NativeServiceStatus::unavailable};
            dma_output_.assign(ram.begin()+buffer.offset,ram.begin()+buffer.offset+size);
            events_->post(44);
        } else {
            for (unsigned address=buffer.offset;address<buffer.offset+size;address+=4)
                if (nt_iop_native_code_word(address)) return {NativeServiceStatus::code_write,0,address};
            buffer.armed=true;
        }
        return {NativeServiceStatus::returned};
    }
    std::array<Buffer,2> buffers_{};
    std::vector<std::uint8_t> dma_output_;
    std::array<std::uint32_t,16> commands_{};
    std::array<std::uint32_t,8> ports_{};
    std::uint32_t control_=0,interrupts_=0;
    std::vector<std::uint8_t> output_;
    std::vector<std::uint8_t> input_;
    std::size_t cursor_=0;
    std::uint32_t status_=0;
    bool completed_=false;
    NativeInterruptServices* events_=nullptr;
    NativePortHandler handler_;
    unsigned failure_=0,expected_output_=0,expected_input_=0;
};
}
