#include <nt/ee_pad_rpc.hpp>
#include <nt/pad_transactions.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool protected_code(std::uint32_t address,std::uint32_t size) {
    return address<0x1100 && std::uint64_t(address)+size>0x1000;
}
}
int main(int argc,char**) {
    try {
        using nt::ee::Stop;
        {
            nt::input::PadSnapshot host{true,0x4008,{0,255,32,224},true};
            std::array<std::uint16_t,2> motors{};
            bool motor_ok=true;
            nt::input::PadTransactions pad([&](unsigned port) { return port==0 ? host : nt::input::PadSnapshot{}; },
                [&](unsigned,std::uint16_t low,std::uint16_t high) { motors={low,high}; return motor_ok; });
            std::array<std::uint8_t,9> request{1,0x42},response{};
            nt::NativePortExchange item{0,request,response};
            auto exchange=[&] { return pad.exchange({&item,1}); };
            check(exchange() && item.acknowledged && response[1]==0x41 && response[3]==0xf7 && response[4]==0xbf,
                  "Compiled driver digital report must use host buttons");
            request[1]=0x43; request[3]=1;
            check(exchange() && response[1]==0x41,"Configuration entry reports previous mode");
            request[1]=0x45;
            check(exchange() && response[1]==0xf3 && response[3]==1 && response[6]==2,
                  "Native analog profile must not claim pressure sensors");
            request[1]=0x44; request[3]=1; request[4]=3;
            check(exchange(),"Select analog mode");
            request[1]=0x4d; request[3]=1; request[4]=0;
            std::fill(request.begin()+5,request.end(),0xff);
            check(exchange() && response[3]==0xff,"Configure native rumble slots");
            request[1]=0x43; request[3]=0;
            check(exchange() && response[1]==0xf3,"Exit configuration");
            request[1]=0x42; request[3]=128; request[4]=1;
            check(exchange() && response[1]==0x73 && response[5]==0 && response[6]==255 && response[7]==32 && response[8]==224 &&
                  motors==std::array<std::uint16_t,2>{32896,65535},"Analog polling and swapped actuator mapping");
            motor_ok=false; response.fill(0xcc);
            check(!exchange() && response[0]==0xcc,"Rumble failure must not publish a successful packet");
            motor_ok=true; host.connected=false;
            check(exchange() && !item.acknowledged && response[1]==255,"Disconnected host must not acknowledge");
            host.connected=true;
            check(exchange() && response[1]==0x41,"Reconnect resets negotiated analog mode");
            request[1]=0x43; request[3]=1; check(exchange(),"Reenter config");
            auto bad=request; bad[1]=0xee;
            std::array<std::uint8_t,9> other{};
            request[1]=0x44; request[3]=1; request[4]=3;
            std::array<nt::NativePortExchange,2> batch{{{0,request,response},{0,bad,other}}};
            response.fill(0xcc);
            check(!pad.exchange(batch) && response[0]==0xcc && pad.rejected_command()==0xee,
                  "Unsupported batch must not commit earlier commands");
            request[1]=0x45;
            check(exchange() && response[5]==0,"Rejected batch changed mode");
            item.port=1; check(exchange() && !item.acknowledged,"Absent second port must remain absent");
            item.port=0; request[0]=0x81;
            check(exchange() && !item.acknowledged,"Controller cannot acknowledge a memory card request");
        }
        check(nt::input::pad_axis(-32768)==0 && nt::input::pad_axis(0)==128 && nt::input::pad_axis(32767)==255,"Axis normalization");
        std::vector<std::uint8_t> ram(0x10000,0xcc);
        nt::ee::Memory memory{}; memory.data=ram.data(); memory.size=ram.size();
        nt::input::PadSnapshot snapshot;
        std::array<std::uint16_t,2> motors{};
        bool motor_success=true;
        nt::ee::NativePadRpc service([&](unsigned port) { return port==0 ? snapshot : nt::input::PadSnapshot{}; },
            [&](unsigned port,std::uint16_t low,std::uint16_t high) {
                check(port==0,"Unexpected motor port");
                if (motor_success) motors={low,high};
                return motor_success;
            });
        std::array<std::uint8_t,128> args{};
        auto put=[&](unsigned offset,std::uint32_t value) { std::memcpy(args.data()+offset,&value,4); };
        auto call=[&](std::uint32_t command) { put(0,command); return service.call(memory,protected_code,1,args,0x3000,128); };
        put(16,0x2000);
        check(call(0x80000100)==Stop::budget && nt::le32(ram,0x300c)==1,"Open creates subscription");
        check(ram[0x2004]==0 && nt::le32(ram,0x2028)==0,"No device must remain disconnected");
        snapshot.connected=true; snapshot.buttons=(1U<<14)|1U; snapshot.axes={0,255,32,224};
        check(service.update(memory,protected_code)==Stop::budget && ram[0x2004]==6,"Connect publication");
        check(nt::le32(ram,0x2028)==4 && ram[0x2009]==0x41 && ram[0x200a]==0xfe && ram[0x200b]==0xbf,"Digital active-low packet");
        put(12,4); put(16,0xffffffff);
        check(call(0x80000104)==Stop::budget && nt::le32(ram,0x3014)==2,"Analog mode table");
        put(12,1); put(16,3);
        check(call(0x80000105)==Stop::budget && nt::le32(ram,0x3014)==1,"Apply analog mode");
        ram[0x2005]=ram[0x2045]=2;
        check(service.update(memory,protected_code)==Stop::budget && ram[0x2005]==0 && ram[0x2045]==0,"Complete client busy state");
        check(ram[0x2009]==0x73 && nt::le32(ram,0x2028)==8 && ram[0x200c]==0 && ram[0x200d]==255 && ram[0x200e]==32 && ram[0x200f]==224,"Analog report order");
        check(std::equal(ram.begin()+0x2000,ram.begin()+0x2040,ram.begin()+0x2040),"Consistent double buffer");
        snapshot.rumble=true;
        put(12,0xffffffff); put(16,0);
        check(call(0x80000102)==Stop::budget && nt::le32(ram,0x3014)==2,"Enumerate supported motors");
        args[12]=1; args[13]=0; std::fill(args.begin()+14,args.begin()+18,0xff);
        check(call(0x80000107)==Stop::budget && nt::le32(ram,0x3014)==1,"Configure swapped motor alignment");
        args[12]=128; args[13]=1;
        check(call(0x80000106)==Stop::budget && nt::le32(ram,0x3014)==1 && motors==std::array<std::uint16_t,2>{32896,65535},"Map rumble amplitudes");
        motor_success=false; args[12]=0; args[13]=0;
        check(call(0x80000106)==Stop::budget && nt::le32(ram,0x3014)==0 && motors[0]==32896,"Host failure is not success");
        motor_success=true;
        check(call(0x80000106)==Stop::budget && motors==std::array<std::uint16_t,2>{0,0},"Stop motors");
        args[12]=0; args[13]=0;
        check(call(0x80000107)==Stop::invalid_state,"Reject duplicate motor mappings");
        snapshot={};
        check(service.update(memory,protected_code)==Stop::budget && ram[0x2004]==0 && ram[0x200a]==255 && nt::le32(ram,0x2028)==0,"Disconnect clears held input");
        const auto before=ram;
        check(call(0x80000109)==Stop::unsupported_system && ram==before,"Unsupported pressure mode must not succeed");
        put(4,1); put(16,0x1000);
        check(call(0x80000100)==Stop::code_write && ram==before,"Open protects compiled code");
        put(16,0xffc0);
        check(call(0x80000100)==Stop::memory_fault && ram==before,"Open bounds check");
        put(16,0x2040);
        check(call(0x80000100)==Stop::invalid_state && ram==before,"Overlapping port buffers");
        put(16,0x80004000);
        check(call(0x80000100)==Stop::budget && ram[0x4004]==0,"Cached alias accepted");
        put(4,0);
        check(call(0x8000010d)==Stop::budget,"Close subscription");
        ram[0x2000]=0xaa; snapshot.connected=true;
        check(service.update(memory,protected_code)==Stop::budget && ram[0x2000]==0xaa,"Closed buffer no longer written");
        if (argc>1) {
            nt::input::Gamepads devices;
            for (unsigned port=0;port<2;++port) std::cout<<"port="<<port<<" connected="<<devices.sample(port).connected<<" name="<<devices.name(port)<<'\n';
        }
        std::cout<<"Native pad lifecycle, packets, aliases and protection passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
