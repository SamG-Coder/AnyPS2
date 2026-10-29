#include <nt/iop_dma.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
namespace {
std::uint32_t interrupt_value(std::uint32_t value) {
    const bool active=(value&0x8000) || ((value&0x800000) && ((value>>8)&value&0x7f0000));
    return (value&0x7fffffffU)|(active ? 0x80000000U : 0);
}
}
bool DmaController::interrupt_pending() const {
    return forced_interrupt() || pending_channels();
}
std::uint32_t DmaController::pending_channels() const {
    std::uint32_t pending=0;
    for (unsigned bank=0;bank<2;++bank) {
        const auto value=interrupts_[bank];
        const auto enables=((value>>16)&0x7f)|(bank==1 ? 0xcU : 0);
        pending|=((value>>24)&enables&(bank==1 ? 0x3fU : 0x7fU))<<(bank*7);
    }
    return pending;
}
bool DmaController::signal_completion(unsigned channel) {
    if (channel>=channels_.size()) return false;
    const auto bank=channel<7 ? 0U : 1U, index=channel<7 ? channel : channel-7;
    auto& value=interrupts_[bank];
    // SIF0/1 tag interrupt sources remain active independently of DMA-end masks.
    if ((value&(1U<<(16+index))) || channel==9 || channel==10) value|=1U<<(24+index);
    return true;
}
std::optional<std::uint32_t> DmaController::channel_register(unsigned channel,unsigned offset) {
    if (channel>=13 || offset%4 || offset>12 || (offset==12 && channel!=4 && channel!=9)) return {};
    return (channel<7 ? 0x1f801080+channel*16 : 0x1f801500+(channel-7)*16)+offset;
}
std::optional<std::uint32_t> DmaController::read(std::uint32_t address,unsigned bytes) const {
    if ((bytes!=2 && bytes!=4) || address%bytes) return {};
    if (address==0x1f8010f0 && bytes==4) return priority_;
    if (address==0x1f801570 && bytes==4) return priority2_;
    if (bytes==4 && (address==0x1f8010f4 || address==0x1f801574))
        return interrupt_value(interrupts_[address==0x1f801574]);
    for (unsigned channel=0;channel<channels_.size();++channel) {
        const auto base=*channel_register(channel,0);
        if (address>=base && address-base<16 && channel_register(channel,(address-base)&~3U)) {
            const auto value=channels_[channel][(address-base)/4];
            return bytes==4 ? value : (value>>((address&2)*8))&0xffff;
        }
    }
    return {};
}
bool DmaController::write(std::uint32_t address,unsigned bytes,std::uint32_t value) {
    if ((bytes!=2 && bytes!=4) || address%bytes) return false;
    if (address==0x1f8010f0 && bytes==4) { priority_=value; return true; }
    if (address==0x1f801570 && bytes==4) { priority2_=value; return true; }
    if (bytes==4 && (address==0x1f8010f4 || address==0x1f801574)) {
        auto& previous=interrupts_[address==0x1f801574];
        previous=((previous&0x7f000000)&~value)|(value&0xffffff);
        return true;
    }
    for (unsigned channel=0;channel<channels_.size();++channel) {
        const auto base=*channel_register(channel,0);
        if (address<base || address-base>=16 || !channel_register(channel,(address-base)&~3U)) continue;
        auto& reg=channels_[channel][(address-base)/4];
        if (bytes==2) {
            const auto shift=(address&2)*8;
            value=(reg&~(0xffffU<<shift))|((value&0xffff)<<shift);
        }
        const auto offset=(address-base)&~3U;
        if (channel==10 && offset!=8 && ((channels_[10][2]&0x01000000) || sif1_packet_)) return false;
        // Only the connected SIF1 destination-chain receiver can accept START.
        if (offset==8 && (value&0x01000000) &&
            (channel!=10 || !sif1_connected_ || (value&~0x40000000U)!=0x01000300)) return false;
        reg=value;
        return true;
    }
    return false;
}
bool DmaController::receive_sif1(std::deque<std::uint32_t>& fifo,std::span<std::uint8_t> ram) {
    auto& channel=channels_[10];
    if (!sif1_connected_ || !(channel[2]&0x01000000) || !(priority2_&0x8000)) return false;
    if (!sif1_packet_) {
        if (fifo.size()<4) return false;
        const auto destination=fifo[0]&0xffffff, words=fifo[1]&0xffffc;
        if (destination%4 || destination>ram.size() || std::uint64_t(words)*4>ram.size()-destination)
            throw std::runtime_error("IOP SIF1: invalid destination extent");
        channel[0]=destination;
        sif1_remaining_=words;
        sif1_end_=(fifo[0]&0xc0000000U)!=0;
        sif1_packet_=true;
        for (unsigned i=0;i<4;++i) fifo.pop_front();
    } else if (sif1_remaining_) {
        const auto count=static_cast<unsigned>(std::min<std::size_t>({4,fifo.size(),sif1_remaining_}));
        // Destination extent was validated atomically before consuming the tag.
        for (unsigned i=0;i<count;++i) {
            const auto word=fifo.front(); fifo.pop_front();
            for (unsigned byte=0;byte<4;++byte) ram[channel[0]+byte]=static_cast<std::uint8_t>(word>>(8*byte));
            channel[0]+=4;
        }
        sif1_remaining_-=count;
    }
    if (sif1_remaining_) return false;
    sif1_packet_=false;
    if (sif1_end_) {
        channel[2]&=~0x01000000U;
        signal_completion(10);
    }
    return true;
}
}
