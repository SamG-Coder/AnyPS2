#include "disc_fixture.hpp"
#include <nt/ee_file_rpc.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <nt/ee_bootstrap_modules.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_services.hpp>
#include <chrono>
#include <cstring>
#include <source_location>

extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
namespace {
void require(bool value,std::source_location at=std::source_location::current()) {
    if (!value) throw std::runtime_error("Native file RPC lifecycle failed at "+std::to_string(at.line()));
}
nt::Bytes words(std::initializer_list<std::uint32_t> values) {
    nt::Bytes bytes(values.size()*4); unsigned offset=0;
    for (auto value:values) { fixture::put32(bytes,offset,value); offset+=4; }
    return bytes;
}
struct TemporaryDisc {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("anyps2-native-rpc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    ~TemporaryDisc() { std::error_code ignored; std::filesystem::remove(path,ignored); }
};
}
void run_ee_file_rpc_tests() {
    using nt::ee::Stop;
    TemporaryDisc disc;
    const nt::Bytes data{11,22,33,44,55,66,77};
    nt::write_file(disc.path,fixture::single_file_iso("DATA.BIN;1",data));
    nt::ee::NativeFileRpc files(disc.path);
    nt::ee::NativeCdvdInitRpc cdvd(disc.path);
    {
        std::vector<std::uint8_t> reset_ram(0x10000);
        nt::ee::Memory reset_memory{reset_ram.data(),reset_ram.size()};
        nt::ee::NativePlatform reset_platform; reset_platform.attach(reset_memory);
        reset_platform.completions.enable(5,true);
        nt::ee::NativeCdvdInitRpc reset_cdvd(disc.path,0x8000,0x9000);
        nt::ee::NativeSifCommandPeer dispatcher(reset_platform.sif_link,reset_memory,nt_ee_generated_code_overlap,0x1000,&reset_platform.sif_receive);
        require(dispatcher.register_service(reset_cdvd));
        auto command=words({56U|(4U<<8),0x9000,0x8000000a,0,0,9,0,0x7000,0,4,0x5000,16,1,0x8000});
        command.resize(64);
        const std::array<nt::ee::NativeSifPacket,2> request{{{0x9000,0,std::vector<std::uint8_t>(16)}, {0x1000,0x44,command}}};
        require(dispatcher.owns_rpc(request) && !reset_cdvd.initialized());
        require(dispatcher.dispatch_rpc(request,0x6000,false).status==Stop::unsupported_system);
        require(dispatcher.dispatch_rpc(request,0x6000,true).value==0 && !reset_cdvd.initialized());
        reset_platform.sif_receive.write(0x1000c000,0x184);
        auto malformed=request; malformed[0].destination=0x9010;
        require(dispatcher.dispatch_rpc(malformed,0x6000,true).status==Stop::invalid_state && !reset_cdvd.initialized());
        const auto completed=dispatcher.dispatch_rpc(request,0x6000,true);
        require(completed.status==Stop::budget && completed.value==1 && reset_cdvd.initialized() &&
                nt::le32(reset_ram,0x5000)==1 && nt::le32(reset_ram,0x6008)==0x80000008 &&
                reset_platform.completions.ready()==32 && !reset_platform.sif_link.peer_flags());
        // The RPC dispatcher never publishes handshake or reset readiness.
    }
    nt::ee::NativeRpcService* endpoint=&files;
    std::vector<std::uint8_t> ram(0x10000,0xcc);
    nt::ee::Memory memory{ram.data(),ram.size()};
    nt::ee::NativePlatform platform; platform.attach(memory);
    nt::ee::NativeSifCommandPeer peer(platform.sif_link,memory,nt_ee_generated_code_overlap,0x1000,&platform.sif_receive);
    require(peer.register_service(files) && !peer.register_service(files) && peer.register_service(cdvd) && peer.connect(platform.sif_outbound));
    nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
    auto state=nt::ee::NativePlatform::initial_state(0x3a0000);
    state.set(4,5); state.set(5,0x3c0000); state.set(6,0); state.set(7,0); state.set(28,0x7000);
    require(platform.interrupts.service(0x12,state,nt_ee_generated_contains,&platform.completions).status==Stop::budget);
    platform.completions.enable(5,true);
    const auto arm=[&] { require(platform.sif_receive.write(0x1000c000,0x184)==Stop::budget); };
    const auto finish=[&] {
        require(platform.completions.ready()==32);
        state.pc=0x3a0000; state.cop0_status=0x10c01; state.set(29,0xff00);
        require(nt::ee::run(nt_ee_generated_run,state,memory,5,services)==Stop::budget);
        require(state.pc==0x3a0000 && state.cop0_status==0x10c01 && nt::le32(ram,0x7000)==5);
        require(!platform.completions.ready() && platform.sif_receive.accepting());
        platform.sif_receive.stop();
    };
    const auto submit=[&](nt::Bytes packet,nt::Bytes args=nt::Bytes{}) {
        const auto size=packet.size(); packet.resize((size+15)&~std::size_t(15));
        std::memcpy(ram.data()+0x4000,packet.data(),packet.size());
        nt::Bytes descriptors;
        if (!args.empty()) {
            const auto logical=args.size(); args.resize((logical+15)&~std::size_t(15));
            std::memcpy(ram.data()+0x9000,args.data(),args.size());
            descriptors=words({0x9000,endpoint->buffer,static_cast<std::uint32_t>(logical),0});
        }
        auto command=words({0x4000,0x1000,static_cast<std::uint32_t>(size),0x44});
        descriptors.insert(descriptors.end(),command.begin(),command.end());
        std::memcpy(ram.data()+0x3000,descriptors.data(),descriptors.size());
        const auto result=platform.sif_outbound.submit(memory,0x3000,static_cast<std::uint32_t>(descriptors.size()/16));
        require(result.status==Stop::budget && result.value);
        return result.value;
    };
    submit(words({20,0,0x80000002,0,0x6000})); require(platform.sif_outbound.pump(memory)==Stop::budget);
    arm(); submit(words({16,0,0x80000002,1})); require(platform.sif_outbound.pump(memory)==Stop::budget); finish();
    {
        nt::ee::NativeBootstrapModules modules;
        peer.data_reader([&](std::uint32_t address,std::span<std::uint8_t> output) { return modules.snapshot(address,output); });
        auto read=words({64U|(4U<<8),0,0x8000000c,0,7,9,11,13,0x800,0x5100,48}); read.resize(64);
        // A reused SDK command can retain dsize despite no extra transfer.
        const auto pending=submit(read);
        require(platform.sif_outbound.pump(memory)==Stop::budget && platform.sif_outbound.status(pending)==1 && ram[0x5100]==0xcc);
        arm(); require(platform.sif_outbound.pump(memory)==Stop::budget);
        require(std::all_of(ram.begin()+0x5100,ram.begin()+0x5130,[](auto byte) { return byte==0; }) &&
            nt::le32(ram,0x6010)==7 && nt::le32(ram,0x6014)==9 && nt::le32(ram,0x6020)==0x8000000c);
        finish();
        nt::Bytes outside(4,0xcc);
        require(modules.snapshot(0x7ff,outside)==Stop::unsupported_system && outside==nt::Bytes(4,0xcc));
        nt::Bytes load(512,0); const std::string rom="rom0:controller";
        std::copy(rom.begin(),rom.end(),load.begin()+8);
        require(modules.call(memory,nt_ee_generated_code_overlap,0,load,0x5100,8)==Stop::budget &&
            nt::le32(ram,0x5100)==static_cast<std::uint32_t>(-203));
        const auto unchanged=ram;
        require(modules.call(memory,nt_ee_generated_code_overlap,0,load,0x1000,8)==Stop::code_write);
        load[8]='x';
        require(modules.call(memory,nt_ee_generated_code_overlap,0,load,0x5100,8)==Stop::unsupported_system && ram==unchanged);
        peer.data_reader({});
    }
    arm(); submit(words({36U|(4U<<8),0,0x80000009,0,7,9,11,13,files.id}));
    require(platform.sif_outbound.pump(memory)==Stop::budget);
    require(nt::le32(ram,0x6024)==files.server && nt::le32(ram,0x6028)==files.buffer); finish();
    const auto request=[&](unsigned method,const nt::Bytes& args,unsigned destination=0x5004,unsigned size=4) {
        return submit(words({64U|(static_cast<unsigned>(args.size())<<8),args.empty() ? 0 : endpoint->buffer,
            0x8000000a,0,17,19,23,29,method,static_cast<unsigned>(args.size()),destination,size,1,endpoint->server,0,0}),args);
    };
    const auto call=[&](unsigned method,const nt::Bytes& args,unsigned size=4) {
        arm(); request(method,args,0x5004,size); require(platform.sif_outbound.pump(memory)==Stop::budget);
        require(nt::le32(ram,0x6010)==17 && nt::le32(ram,0x6014)==19 && nt::le32(ram,0x6018)==23 &&
            nt::le32(ram,0x601c)==29 && nt::le32(ram,0x6020)==0x8000000a);
        const auto result=nt::le32(ram,0x5004); finish(); return result;
    };
    nt::Bytes open=words({1}); const std::string name="cdrom0:\\DATA.BIN;1";
    open.insert(open.end(),name.begin(),name.end()); open.push_back(0);
    const auto pending=request(0,open);
    require(platform.sif_outbound.pump(memory)==Stop::budget && files.statistics().requests==0 && platform.sif_outbound.status(pending)==1);
    // Invalid receive extent cannot consume a file handle, then retry the same snapshot.
    arm(); memory.size=0x6001;
    require(platform.sif_outbound.pump(memory)==Stop::memory_fault && files.statistics().requests==0);
    memory.size=ram.size(); require(platform.sif_outbound.pump(memory)==Stop::budget);
    const auto fd=nt::le32(ram,0x5004); require(fd==3 && files.statistics().opens==1); finish();
    require(call(2,words({fd,0x80008001,5,0x8100}))==5);
    require(std::equal(data.begin(),data.begin()+5,ram.begin()+0x8001) && ram[0x8000]==0xcc && ram[0x8006]==0xcc);
    require(nt::le32(ram,0x8100)==0 && nt::le32(ram,0x810c)==0);
    const auto requests=files.statistics().requests;
    require(files.call(memory,nt_ee_generated_code_overlap,2,words({fd,0x1000,2,0}),0x5004,4)==Stop::code_write);
    require(files.statistics().requests==requests);
    require(call(2,words({fd,0x8008,5,0}))==2 && ram[0x8008]==66 && ram[0x8009]==77 && ram[0x800a]==0xcc);
    require(call(2,words({fd,0x8008,5,0}))==0);
    require(call(4,words({fd,0,0}))==0);
    require(call(255,{},8)==0x30303031 && nt::le32(ram,0x5008)==0x2e2e2e2e);
    require(call(1,words({fd}))==0 && call(2,words({fd,0x8000,4,0}))==0xfffffff7U);
    arm(); request(0,open); const auto stats=files.statistics().requests;
    // Submit owns a snapshot, so corrupting caller RAM cannot change its request.
    ram[0x9004]='X'; require(platform.sif_outbound.pump(memory)==Stop::budget);
    require(nt::le32(ram,0x5004)==3 && files.statistics().requests==stats+1); finish();
    endpoint=&cdvd;
    arm(); submit(words({36,0,0x80000009,0,7,9,11,13,cdvd.id}));
    require(platform.sif_outbound.pump(memory)==Stop::budget);
    require(nt::le32(ram,0x6024)==cdvd.server && nt::le32(ram,0x6028)==cdvd.buffer); finish();
    require(!cdvd.initialized());
    for (const auto mode : {1U,0U,0U,5U,5U,0U}) {
        require(call(0,words({mode}),16)==1 && cdvd.initialized()==(mode!=5));
        require(nt::le32(ram,0x5008)==0 && nt::le32(ram,0x500c)==0 && nt::le32(ram,0x5010)==0);
    }
    const auto fixed_clock=[] {
        using namespace std::chrono;
        return sys_seconds{sys_days{year{2024}/February/28}+hours{15}+minutes{4}+seconds{59}};
    };
    nt::ee::NativeCdvdScmdRpc scmd(cdvd,0xa000,0xb000,fixed_clock);
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,{},0x5004,16)==Stop::budget);
    const std::array<std::uint8_t,16> clock_result{1,0,0,0,0,0x59,0x04,0,0,0x29,0x02,0x24};
    require(std::equal(clock_result.begin(),clock_result.end(),ram.begin()+0x5004));
    // Host time is available to standalone ELF programs without pretending a
    // disc has been mounted or enabling media-dependent operations.
    nt::ee::NativeCdvdInitRpc no_disc(std::filesystem::path{});
    nt::ee::NativeCdvdScmdRpc standalone_clock(no_disc,0xc000,0xd000,fixed_clock);
    require(standalone_clock.call(memory,nt_ee_generated_code_overlap,1,{},0x5004,16)==Stop::budget);
    require(std::equal(clock_result.begin(),clock_result.end(),ram.begin()+0x5004));
    const auto no_disc_bytes=ram;
    require(standalone_clock.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5040,4)==Stop::unsupported_system);
    require(!no_disc.initialized() && no_disc.media_mode()==0 && ram==no_disc_bytes);
    const auto before=ram;
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,{},0x1000,16)==Stop::code_write);
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,{},0xffff,16)==Stop::memory_fault);
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,words({0}),0x5004,16)==Stop::invalid_state);
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,{},0x5004,4)==Stop::invalid_state);
    require(scmd.call(memory,nt_ee_generated_code_overlap,2,{},0x5004,16)==Stop::unsupported_system);
    require(ram==before);
    require(cdvd.call(memory,nt_ee_generated_code_overlap,0,words({5}),0x1000,16)==Stop::code_write);
    require(cdvd.call(memory,nt_ee_generated_code_overlap,0,words({5}),0xffff,16)==Stop::memory_fault);
    require(cdvd.call(memory,nt_ee_generated_code_overlap,0,words({5}),0x5004,8)==Stop::invalid_state);
    require(cdvd.call(memory,nt_ee_generated_code_overlap,1,words({5}),0x5004,16)==Stop::unsupported_system);
    require(cdvd.call(memory,nt_ee_generated_code_overlap,0,words({2}),0x5004,16)==Stop::unsupported_system);
    require(cdvd.initialized() && ram==before);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5040,4)==Stop::budget);
    require(cdvd.media_mode()==2 && nt::le32(ram,0x5040)==1 && scmd.last_argument()==2);
    require(scmd.call(memory,nt_ee_generated_code_overlap,1,{},0x5044,16)==Stop::budget);
    require(cdvd.media_mode()==2 && std::equal(clock_result.begin(),clock_result.end(),ram.begin()+0x5044));
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({1}),0x5040,4)==Stop::budget);
    require(cdvd.media_mode()==1);
    const auto media_bytes=ram;
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({0}),0x5040,4)==Stop::unsupported_system);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({0xff}),0x5040,4)==Stop::unsupported_system);
    require(cdvd.media_mode()==1 && scmd.last_argument()==0xff);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x1000,4)==Stop::code_write);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({2}),0xffff,4)==Stop::memory_fault);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,{},0x5040,4)==Stop::invalid_state);
    require(scmd.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5040,16)==Stop::invalid_state);
    require(cdvd.media_mode()==1 && ram==media_bytes && scmd.last_argument()==2);
    nt::ee::NativeCdvdInitRpc closed(disc.path);
    nt::ee::NativeCdvdScmdRpc closed_commands(closed,0xc000,0xd000);
    require(closed_commands.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5060,4)==Stop::unsupported_system);
    require(!closed.initialized() && closed.media_mode()==0 && ram==media_bytes);
    require(closed.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5060,4)==Stop::budget);
    require(closed_commands.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5060,4)==Stop::budget);
    require(closed.media_mode()==2 && nt::le32(ram,0x5060)==1);
    require(closed.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5060,4)==Stop::budget);
    require(closed.initialized() && closed.media_mode()==2);
    require(closed.call(memory,nt_ee_generated_code_overlap,0,words({5}),0x5060,4)==Stop::budget);
    require(!closed.initialized() && closed.media_mode()==0);
    require(closed_commands.call(memory,nt_ee_generated_code_overlap,34,words({2}),0x5060,4)==Stop::unsupported_system);
    require(closed.media_mode()==0);
    nt::ee::NativeCdvdReadyRpc ready(cdvd,0x8000059AU,0xc000,0xd000);
    nt::ee::NativeCdvdReadyRpc ready_alternate(closed,0x8000059CU,0xc100,0xd100);
    require(ready.call(memory,nt_ee_generated_code_overlap,0,words({1}),0x5080,4)==Stop::budget);
    require(nt::le32(ram,0x5080)==2 && ready.last_result()==2 && cdvd.media_mode()==1);
    require(ready.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5080,4)==Stop::budget);
    require(nt::le32(ram,0x5080)==2);
    require(ready.call(memory,nt_ee_generated_code_overlap,0,words({8}),0x5080,4)==Stop::budget);
    require(nt::le32(ram,0x5080)==0x40);
    require(ready.call(memory,nt_ee_generated_code_overlap,0,words({3}),0x5080,4)==Stop::budget);
    require(nt::le32(ram,0x5080)==2 && ready.calls()==4);
    const auto ready_bytes=ram;
    require(ready.call(memory,nt_ee_generated_code_overlap,0,words({1}),0x1000,4)==Stop::code_write);
    require(ready.call(memory,nt_ee_generated_code_overlap,0,{},0x5080,4)==Stop::invalid_state);
    require(ready.call(memory,nt_ee_generated_code_overlap,1,words({1}),0x5080,4)==Stop::unsupported_system);
    require(ram==ready_bytes && nt::le32(ram,0x5080)==2);
    require(ready_alternate.call(memory,nt_ee_generated_code_overlap,0,words({1}),0x5084,4)==Stop::budget);
    require(nt::le32(ram,0x5084)==6 && ready_alternate.last_mode()==1);
    require(ready_alternate.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5084,4)==Stop::budget);
    require(nt::le32(ram,0x5084)==2);
    require(ready_alternate.call(memory,nt_ee_generated_code_overlap,0,words({8}),0x5084,4)==Stop::budget);
    require(nt::le32(ram,0x5084)==1);
    nt::ee::NativeCdvdInitRpc missing(disc.path.string()+".missing");
    const auto missing_bytes=ram;
    require(missing.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5004,16)==Stop::unsupported_system);
    require(!missing.initialized() && !missing.error().empty() && ram==missing_bytes);
    TemporaryDisc truncated;
    auto incomplete=fixture::single_file_iso("DATA.BIN;1",data);
    incomplete.resize(17*2048); // Descriptor exists, root directory extent does not.
    nt::write_file(truncated.path,incomplete);
    nt::ee::NativeCdvdInitRpc bad_media(truncated.path);
    require(bad_media.call(memory,nt_ee_generated_code_overlap,0,words({1}),0x5004,16)==Stop::budget);
    require(bad_media.initialized());
    const auto media_before=ram;
    require(bad_media.call(memory,nt_ee_generated_code_overlap,0,words({0}),0x5004,16)==Stop::unsupported_system);
    require(bad_media.initialized() && ram==media_before && !bad_media.error().empty());
    require(bad_media.call(memory,nt_ee_generated_code_overlap,0,words({5}),0x5004,4)==Stop::budget);
    require(!bad_media.initialized() && bad_media.error().empty());
    endpoint=&files;
    // Malformed payload size fails without publishing a result or completion.
    arm(); submit(words({64U|(8U<<8),files.buffer,0x8000000a,0,17,19,23,29,1,4,0x5004,4,1,files.server,0,0}),words({3}));
    const auto unchanged=ram; const auto completed=files.statistics().requests;
    require(platform.sif_outbound.pump(memory)==Stop::invalid_state && ram==unchanged && !platform.completions.ready() && files.statistics().requests==completed);
    {
        nt::Bytes output;
        unsigned output_fd=0,calls=0;
        nt::ee::NativeFileRpc standalone(std::filesystem::path{},[&](std::uint32_t fd,nt::ByteView bytes) {
            output_fd=fd; ++calls;
            output.insert(output.end(),bytes.begin(),bytes.end());
            return static_cast<std::int32_t>(bytes.size());
        });
        auto write=words({1,0x8000800d,5,3}); write.resize(32);
        write[16]='a'; write[17]='b'; write[18]='c';
        ram[0x8010]='d'; ram[0x8011]='e';
        require(standalone.call(memory,nt_ee_generated_code_overlap,3,write,0x5004,4)==Stop::budget);
        require(output==nt::Bytes({'a','b','c','d','e'}) && output_fd==1 && calls==1 && nt::le32(ram,0x5004)==5);
        // Bytes in the request own the prefix, while the aligned remainder is
        // read from actual native memory, including its uncached address alias.
        const auto valid_write=write,snapshot=ram;
        require(standalone.call(memory,nt_ee_generated_code_overlap,3,write,0x1000,4)==Stop::code_write);
        fixture::put32(write,12,6);
        require(standalone.call(memory,nt_ee_generated_code_overlap,3,write,0x5004,4)==Stop::invalid_state);
        write=valid_write; fixture::put32(write,4,0xffff);
        require(standalone.call(memory,nt_ee_generated_code_overlap,3,write,0x5004,4)==Stop::memory_fault);
        require(calls==1 && ram==snapshot);
        write=valid_write; fixture::put32(write,0,2); fixture::put32(write,8,0); fixture::put32(write,12,0);
        require(standalone.call(memory,nt_ee_generated_code_overlap,3,write,0x5004,4)==Stop::budget);
        require(output_fd==2 && calls==2 && nt::le32(ram,0x5004)==0 && output.size()==5);
        require(standalone.call(memory,nt_ee_generated_code_overlap,0,open,0x5004,4)==Stop::budget);
        require(nt::le32(ram,0x5004)==static_cast<std::uint32_t>(-19));
        nt::ee::NativeFileRpc unavailable(std::filesystem::path{});
        require(unavailable.call(memory,nt_ee_generated_code_overlap,3,valid_write,0x5004,4)==Stop::unsupported_system);
        nt::ee::NativeFileRpc failing(std::filesystem::path{},[](std::uint32_t,nt::ByteView) { return -5; });
        require(failing.call(memory,nt_ee_generated_code_overlap,3,valid_write,0x5004,4)==Stop::budget);
        require(nt::le32(ram,0x5004)==static_cast<std::uint32_t>(-5));
    }
}
