#include <nt/ee_iop_delivery.hpp>
#include <nt/ee_iop_commands.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_services.hpp>
#include <nt/iop_native_sif_commands.hpp>
#include <nt/iop_llvm.hpp>
#include <nt/iop_native_libraries.hpp>
#include <iostream>
#include <source_location>

extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
namespace {
void require(bool condition,std::source_location at=std::source_location::current()) {
    if (!condition) throw std::runtime_error("Native SIF integration failed at "+std::to_string(at.line()));
}
}
int main() {
    try {
        using namespace nt::iop;
        using nt::ee::Stop;
        std::vector<std::uint8_t> ee_ram(0x10000,0xcc),iop_ram(0x10000,0);
        nt::ee::Memory memory{ee_ram.data(),ee_ram.size()};
        nt::ee::NativePlatform platform; platform.attach(memory);
        nt::ee::NativeIopDelivery deliver(memory,nt_ee_generated_code_overlap,platform.sif_receive);
        NativeSifDestination destination{ee_ram,[](auto address,auto size) { return !nt_ee_generated_code_overlap(address,size); }};
        NativeSifTransfers transfers(&destination);
        NativeSifCommandEndpoint endpoint(destination,0x6000,transfers,deliver);
        NativeSifCommands commands(&endpoint);
        auto ee=nt::ee::NativePlatform::initial_state(0x3a0000);
        ee.set(4,5); ee.set(5,0x3c0000); ee.set(6,0); ee.set(7,0); ee.set(28,0x7000);
        require(platform.interrupts.service(0x12,ee,nt_ee_generated_contains,&platform.completions).status==Stop::budget);
        platform.completions.enable(5,true);
        nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains);
        for (unsigned byte=0;byte<19;++byte) iop_ram[0xd200+byte]=static_cast<std::uint8_t>(byte+91);
        store_native_word(iop_ram,0xd310,0x80005000); store_native_word(iop_ram,0xd314,19);
        const auto send=[&]() {
            Cpu cpu; cpu.pc=0x2434; cpu.registers[31]=0x5000; cpu.registers[29]=0xd300;
            cpu.registers[4]=0x8000000a; cpu.registers[5]=0xd100; cpu.registers[6]=17; cpu.registers[7]=0xd200;
            const auto result=run_native(run_llvm,cpu,iop_ram,1,&commands);
            return std::pair{result,cpu};
        };
        const auto original=ee_ram;
        auto sent=send();
        require(sent.first.reason==NativeStop::budget && sent.second.registers[2]==0 && ee_ram==original && !platform.completions.ready());
        require(platform.sif_receive.write(0x1000c000,0x184)==Stop::budget);
        memory.size=0x6001;
        sent=send();
        require(sent.first.reason==NativeStop::address_fault && !sent.second.executed && ee_ram==original && !platform.completions.ready());
        memory.size=ee_ram.size();
        sent=send();
        require(sent.first.reason==NativeStop::budget && sent.second.registers[2]==1 && platform.completions.ready()==32);
        require(load_native_word(ee_ram,0x6000)==(19U<<8|17) &&
                std::equal(iop_ram.begin()+0xd200,iop_ram.begin()+0xd213,ee_ram.begin()+0x5000));
        for (unsigned byte=17;byte<32;++byte) require(ee_ram[0x6000+byte]==0);
        require(!endpoint.take()); // The real receiver consumed it; no second mailbox delivery.
        const auto accepted=ee_ram;
        sent=send();
        require(sent.first.reason==NativeStop::budget && sent.second.registers[2]==0 && ee_ram==accepted);
        ee.pc=0x3a0000; ee.cop0_status=0x10c01; ee.set(29,0xff00);
        require(nt::ee::run(nt_ee_generated_run,ee,memory,5,services)==Stop::budget);
        require(ee.pc==0x3a0000 && load_native_word(ee_ram,0x7000)==5 && !platform.completions.ready() && platform.sif_receive.accepting());
        sent=send();
        require(sent.first.reason==NativeStop::budget && sent.second.registers[2]==2 && platform.completions.ready()==32);
        NativeSifInbound inbound(iop_ram,0xce00);
        NativeInterruptServices interrupts(0xf000,&inbound);
        NativeSifControl control(interrupts,&transfers);
        NativeEventServices events;
        NativeBootServices boot;
        NativeSystemServices system(events,boot,iop_ram);
        NativeSifCommandSession session(events,system,control,&endpoint);
        inbound.connect(commands,session,control);
        nt::ee::NativeIopCommands incoming(control,inbound);
        require(platform.sif_outbound.connect(incoming.sink()));
        // Explicit EE owner publication, followed by actual IOP initialization.
        control.publish_main(0x6000,0x10000);
        require(control.invoke({5,{},NativeServiceKind::sifman},iop_ram).status==NativeServiceStatus::returned);
        require(control.invoke({27,{0xce00},NativeServiceKind::sifman},iop_ram).value==0xce00);
        require(commands.invoke({8,{0xcc00,4},NativeServiceKind::sifcmd},iop_ram).status==NativeServiceStatus::returned);
        NativeServiceRouter iop_services({&commands,&interrupts});
        Cpu registration; registration.pc=0x2480; registration.registers[31]=0x5000;
        require(run_native(run_llvm,registration,iop_ram,32,&iop_services).reason==NativeStop::unmapped_code);
        store_native_word(iop_ram,0xcf00,10);
        store_native_word(ee_ram,0x4000,32); store_native_word(ee_ram,0x4004,0);
        store_native_word(ee_ram,0x4008,1); store_native_word(ee_ram,0x400c,0); ee_ram[0x4010]=7;
        for (const auto [offset,value] : {std::pair{0U,0x4000U},std::pair{4U,0xce00U},std::pair{8U,32U},std::pair{12U,0x44U}})
            store_native_word(ee_ram,0x3000+offset,value);
        require(platform.sif_outbound.submit(memory,0x3000,1).value!=0);
        require(platform.sif_outbound.pump(memory)==Stop::budget && inbound.busy());
        // A second immutable batch must wait until the compiled handler returns.
        const auto second=platform.sif_outbound.submit(memory,0x3000,1).value;
        require(second && platform.sif_outbound.pump(memory)==Stop::budget && platform.sif_outbound.status(second)==1);
        Cpu foreground; foreground.pc=0x5000; foreground.registers[5]=0x12345678;
        require(interrupts.run(run_llvm,foreground,iop_ram,32,iop_services).reason==NativeStop::unmapped_code && !foreground.executed);
        interrupts.invoke({9,{},NativeServiceKind::intrman},iop_ram);
        require(interrupts.run(run_llvm,foreground,iop_ram,32,iop_services).reason==NativeStop::unmapped_code &&
                foreground.executed==7 && foreground.registers[5]==0x12345678 && load_native_word(iop_ram,0xcf00)==17 && !inbound.busy());
        require(platform.sif_outbound.pump(memory)==Stop::budget && platform.sif_outbound.status(second)==0xffffffff);
        require(interrupts.run(run_llvm,foreground,iop_ram,32,iop_services).reason==NativeStop::unmapped_code &&
                load_native_word(iop_ram,0xcf00)==24 && !inbound.busy());
        store_native_word(ee_ram,0x4000,15); // Invalid logical header inside a valid transfer extent.
        require(platform.sif_outbound.submit(memory,0x3000,1).value!=0);
        require(platform.sif_outbound.pump(memory)==Stop::invalid_state && !inbound.busy() &&
                load_native_word(iop_ram,0xcf00)==24);
        std::cout<<"Bidirectional native SIF delivery and compiled EE/IOP handlers passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
