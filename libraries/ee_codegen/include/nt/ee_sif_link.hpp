#pragma once
#include <nt/ee_sif_receive.hpp>
#include <array>

namespace nt::ee {
// Explicit native peer coordination. Only the peer owner may publish peer state.
class NativeSifLink {
public:
    explicit NativeSifLink(NativeSifReceive& receiver) : receiver_(receiver) {}
    NativeSifLink(const NativeSifLink&)=delete;
    NativeSifLink& operator=(const NativeSifLink&)=delete;
    void publish_peer(std::uint32_t address,std::uint32_t flags) { peer_address_=address; peer_flags_|=flags; }
    void replace_peer(std::uint32_t address,std::uint32_t flags) { peer_address_=address; peer_flags_=flags; }
    void begin_peer_reset() { peer_address_=0; peer_flags_=0; reset_clear_pending_=0x30000; }
    bool reset_teardown_complete() const { return reset_clear_pending_==0; }
    std::uint32_t peer_flags() const { return peer_flags_; }
    void acknowledge_main(std::uint32_t flags) { main_flags_&=~flags; }
    std::uint32_t main_address() const { return main_address_; }
    std::uint32_t main_flags() const { return main_flags_; }
    IoRead service(std::uint32_t call,std::uint32_t id,std::uint32_t value) {
        if (call==0x6b) { receiver_.stop(); return {Stop::budget}; }
        if (call==0x78) {
            if (receiver_.accepting()) return {Stop::unsupported_system};
            receiver_.write(0x1000c020,0);
            const auto status=receiver_.write(0x1000c000,0x184);
            if (status==Stop::budget) main_flags_|=0x10000;
            return {status};
        }
        if (call!=0x79 && call!=0x7a) return {Stop::syscall};
        if (id>=0x80000000U && id<0x80000003U) {
            auto& slot=system_[id-0x80000000U];
            if (call==0x79) { slot=value; return {Stop::budget}; }
            return {Stop::budget,slot};
        }
        if (id<1 || id>4) return {Stop::unsupported_system};
        if (call==0x7a) {
            const std::array values{main_address_,peer_address_,main_flags_,peer_flags_};
            return {Stop::budget,values[id-1]};
        }
        if (id==1) main_address_=value;
        else if (id==2) return {Stop::unsupported_system};
        else if (id==3) main_flags_|=value;
        else { peer_flags_&=~value; reset_clear_pending_&=~value; }
        return {Stop::budget};
    }
private:
    NativeSifReceive& receiver_;
    std::array<std::uint32_t,3> system_{};
    std::uint32_t main_address_=0,peer_address_=0,main_flags_=0,peer_flags_=0;
    std::uint32_t reset_clear_pending_=0;
};
}
