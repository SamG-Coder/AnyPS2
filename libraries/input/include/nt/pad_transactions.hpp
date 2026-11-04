#pragma once
#include <nt/gamepad.hpp>
#include <nt/native_port_exchange.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace nt::input {
// Native gamepad service for compiled PADMAN callers. Translate whole command
// packets to host input; no serial clock, device firmware or runtime code decoder.
// Advertise the analog-pad profile: host buttons do not have DS2 pressure sensors.
class PadTransactions {
public:
    using Sampler=std::function<PadSnapshot(unsigned)>;
    using Rumbler=std::function<bool(unsigned,std::uint16_t,std::uint16_t)>;
    PadTransactions(Sampler sample,Rumbler rumble={})
        : sample_(std::move(sample)),rumble_(std::move(rumble)) {}
    std::uint64_t polls() const { return polls_; }
    unsigned rejected_command() const { return rejected_; }
    unsigned rejected_argument() const { return rejected_argument_; }
    bool rejected_config() const { return rejected_config_; }
    bool rejected_rumble() const { return rejected_rumble_; }
    bool exchange(std::span<NativePortExchange> batch) {
        if (batch.size()>16) return false;
        auto next=ports_;
        std::array<PadSnapshot,2> snapshots{};
        for (unsigned port=0;port<2;++port) {
            if (sample_) snapshots[port]=sample_(port);
            if (!snapshots[port].connected) next[port]={};
        }
        std::array<std::array<std::uint8_t,9>,16> replies{};
        std::array<bool,16> acknowledged{};
        struct Motor { unsigned port; std::uint16_t low,high; };
        std::vector<Motor> motors;
        std::uint64_t polls=0;
        for (std::size_t i=0;i<batch.size();++i) {
            const auto& item=batch[i];
            auto& reply=replies[i]; reply.fill(0xff);
            if (item.port>=2 || item.request.empty()) return false;
            const auto& snapshot=snapshots[item.port];
            if (!snapshot.connected || item.request[0]!=1) continue;
            const auto request=item.request;
            if (request.size()<3 || item.response.size()<3) return false;
            const auto command=request[1]; rejected_=command;
            auto& port=next[item.port];
            const bool config=port.config;
            rejected_argument_=request.size()>3 ? request[3] : 0;
            rejected_config_=config; rejected_rumble_=snapshot.rumble && bool(rumble_);
            // Configuration changes the reported mode byte. It is not a gate
            // on supported commands; some drivers query capabilities first.
            reply.fill(0); reply[0]=0xff;
            reply[1]=config ? 0xf3 : port.analog ? 0x73 : 0x41;
            reply[2]=0x5a;
            const auto report=[&] {
                const auto buttons=static_cast<std::uint16_t>(~snapshot.buttons);
                reply[3]=static_cast<std::uint8_t>(buttons);
                reply[4]=static_cast<std::uint8_t>(buttons>>8);
                if (port.analog) std::copy(snapshot.axes.begin(),snapshot.axes.end(),reply.begin()+5);
            };
            if (command==0x42) {
                if (request.size()<5 || item.response.size()<5) return false;
                report(); ++polls;
                std::uint16_t low=0,high=0; bool mapped=false;
                for (unsigned slot=0;slot<6 && slot+3<request.size();++slot) {
                    if (port.alignment[slot]==0) { high=(request[slot+3]&1) ? 65535 : 0; mapped=true; }
                    if (port.alignment[slot]==1) { low=std::uint16_t(request[slot+3])*257; mapped=true; }
                }
                if (mapped) {
                    if (!snapshot.rumble || !rumble_) return false;
                    motors.push_back({item.port,low,high});
                }
            } else if (command==0x43) {
                if (request.size()<4 || request[3]>1) return false;
                if (!config) report();
                port.config=request[3]!=0;
            } else {
                if (request.size()<9 || item.response.size()<9) return false;
                switch (command) {
                case 0x44:
                    if (request[3]>1 || (request[4]!=0 && request[4]!=3)) return false;
                    port.analog=request[3]!=0; port.locked=request[4]==3;
                    break;
                case 0x45:
                    reply[3]=1; reply[4]=2; reply[5]=port.analog;
                    reply[6]=snapshot.rumble && rumble_ ? 2 : 0;
                    reply[7]=reply[6] ? 1 : 0;
                    break;
                case 0x46:
                    if (!snapshot.rumble || !rumble_ || request[3]>1) return false;
                    reply[5]=1; reply[6]=request[3] ? 1 : 2;
                    reply[7]=request[3] ? 1 : 0; reply[8]=request[3] ? 20 : 10;
                    break;
                case 0x47:
                    if (!snapshot.rumble || !rumble_ || request[3]!=0) return false;
                    reply[5]=2; reply[7]=1;
                    break;
                case 0x4c:
                    if (request[3]>1) return false;
                    reply[6]=request[3] ? 7 : 4;
                    break;
                case 0x4d: {
                    unsigned seen=0;
                    for (unsigned slot=0;slot<6;++slot) {
                        const auto motor=request[slot+3];
                        if (motor!=0xff) {
                            if (motor>1 || (seen&(1U<<motor)) || !snapshot.rumble || !rumble_) return false;
                            seen|=1U<<motor;
                        }
                        reply[slot+3]=port.alignment[slot]; port.alignment[slot]=motor;
                    }
                    break;
                }
                default: return false;
                }
            }
            acknowledged[i]=true;
        }
        // Validate the whole batch before changing negotiation or host motors.
        for (const auto& motor:motors) if (!rumble_(motor.port,motor.low,motor.high)) {
            for (const auto& applied:motors) rumble_(applied.port,0,0);
            return false;
        }
        for (std::size_t i=0;i<batch.size();++i) {
            auto& item=batch[i];
            std::fill(item.response.begin(),item.response.end(),0xff);
            if (acknowledged[i]) std::copy_n(replies[i].begin(),std::min(item.response.size(),replies[i].size()),item.response.begin());
            item.acknowledged=acknowledged[i];
        }
        ports_=next; polls_+=polls; rejected_=0;
        return true;
    }
private:
    struct Port {
        bool config=false,analog=false,locked=false;
        std::array<std::uint8_t,6> alignment{0xff,0xff,0xff,0xff,0xff,0xff};
    };
    Sampler sample_;
    Rumbler rumble_;
    std::array<Port,2> ports_{};
    std::uint64_t polls_=0;
    unsigned rejected_=0;
    unsigned rejected_argument_=0;
    bool rejected_config_=false,rejected_rumble_=false;
};
}
