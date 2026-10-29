#include "disc_fixture.hpp"
#include "reboot_fixture.hpp"
#include <nt/iop_cpu.hpp>
#include <nt/iop_libraries.hpp>
#include <nt/iop_startup.hpp>
#include <nt/iop_reboot.hpp>
#include <nt/iop_dependencies.hpp>
#include <nt/iop_intrman.hpp>
#include <nt/iop_registration.hpp>
#include <nt/iop_dmacman.hpp>
#include <nt/iop_heaplib.hpp>
#include <nt/reboot_image.hpp>
#include <algorithm>
#include <iostream>

namespace {
void check(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
void dma_interrupt_dispatch() {
    nt::Bytes ram(0x10000);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman intrman(devices,0x4000); intrman.install(ram,libraries);
    nt::iop::CallRouter calls; calls.add(intrman);
    const auto invoke=[&](unsigned ordinal,std::array<std::uint32_t,4> args) {
        return intrman.invoke(nt::le32(intrman.module().memory,20+ordinal*4),args,ram,false);
    };
    invoke(4,{34,1,0x200,0x1234}); invoke(4,{43,1,0x220,0x5678});
    invoke(6,{34,0,0,0}); invoke(6,{43,0,0,0}); invoke(9,{});
    fixture::put32(ram,0x200,0xac040700); fixture::put32(ram,0x204,0x24020000);
    fixture::put32(ram,0x208,0x03e00008);
    fixture::put32(ram,0x220,0xac040704); fixture::put32(ram,0x224,0x24020001);
    fixture::put32(ram,0x228,0x03e00008);
    devices.signal_dma_completion(2); devices.signal_dma_completion(10);
    nt::iop::Cpu cpu; cpu.pc=0x100; cpu.status=0x401; cpu.registers[29]=0x9000;
    cpu.registers[2]=0xabcd;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          nt::le32(ram,0x700)==0x1234 && devices.pending_dma_channels()==(1U<<10) &&
          !(*devices.read(0x1f8010f4,4)&(1U<<26)) && (*devices.read(0x1f801574,4)&(1U<<27)),
          "DMA dispatch failed to select or acknowledge one channel independently");
    check(nt::iop::run(cpu,ram,4,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x220 && cpu.interrupt_depth==1 && !(*devices.read(0x1f8010f4,4)&(1U<<18)) &&
          !devices.pending_dma_channels(),"DMA chain lost second bank or zero-return mask handling");
    check(nt::iop::run(cpu,ram,5,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          nt::le32(ram,0x704)==0x5678 && cpu.pc==0x100 && cpu.registers[2]==0xabcd &&
          cpu.registers[29]==0x9000 && !cpu.interrupt_depth && !devices.interrupt_pending() &&
          (*devices.read(0x1f801074,4)&8) && (*devices.read(0x1f801574,4)&(1U<<19)),
          "DMA chain did not drain aggregate latch or restore interrupted CPU");
    invoke(6,{36,0,0,0}); devices.signal_dma_completion(4);
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::interrupt_pending &&
          devices.pending_dma_channels()==16,"Unregistered DMA completion was silently discarded");
}
void kernel_mode_callback() {
    nt::Bytes ram(0x10000);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman intrman(devices,0x4000); intrman.install(ram,libraries);
    nt::iop::CallRouter calls; calls.add(intrman);
    const auto entry=nt::le32(intrman.module().memory,20+14*4);
    fixture::put32(ram,0x100,12);
    fixture::put32(ram,0x200,0x00851021); // Sum three callback arguments.
    fixture::put32(ram,0x204,0x00461021);
    fixture::put32(ram,0x208,0xac020700);
    fixture::put32(ram,0x20c,0x03e00008);
    nt::iop::Cpu cpu; cpu.pc=entry; cpu.status=0x401;
    cpu.registers[4]=0x200; cpu.registers[5]=5; cpu.registers[6]=7; cpu.registers[7]=11;
    cpu.registers[29]=0x9000; cpu.registers[31]=0x180;
    cpu.load_register=5; cpu.load_value=13;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x200 && cpu.status==0x404 && cpu.registers[4]==13 && cpu.registers[5]==7 &&
          cpu.registers[6]==11 && cpu.registers[29]==0x9000-28 && !cpu.interrupt_depth,
          "Kernel callback entry lost argument, load-delay or kernel-mode semantics");
    check(nt::iop::run(cpu,ram,6,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==entry+4 && cpu.status==0x401 && cpu.registers[2]==31 &&
          cpu.registers[29]==0x9000 && cpu.registers[31]==0x180 && nt::le32(ram,0x700)==31,
          "Kernel callback return lost result, memory effects or caller continuation");
    nt::iop::run(cpu,ram,2,&devices,&calls);
    check(cpu.pc==0x180,"Native wrapper did not return to its guest caller");
    // A raw syscall can invoke a guest callback that uses the native export again.
    const std::array<std::uint32_t,11> outer{0xafbf0010,0x24040200,0x24050001,0x24060002,
        0x24070003,0x0c000000|(entry>>2),0,0x8fbf0010,0,0x03e00008,0};
    for (unsigned i=0;i<outer.size();++i) fixture::put32(ram,0x300+i*4,outer[i]);
    cpu={}; cpu.pc=0x100; cpu.status=0x401; cpu.registers[2]=0x0c;
    cpu.registers[4]=0x300; cpu.registers[29]=0x9000; cpu.registers[31]=0x180;
    for (unsigned i=0;i<64 && cpu.pc!=0x104;++i)
        check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget,
              "Nested kernel callback failed");
    check(cpu.pc==0x104 && cpu.registers[2]==6 && cpu.status==0x401 &&
          cpu.registers[29]==0x9000 && cpu.registers[31]==0x180,"Nested wrapper context was not restored");
    cpu.pc=0x100; cpu.registers[2]=0x0c; cpu.registers[4]=0xbfc00000;
    const auto before=cpu;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::address_fault &&
          cpu.pc==before.pc && cpu.executed==before.executed && cpu.registers==before.registers,
          "Invalid kernel callback target changed CPU state");
    cpu.registers[4]=0x200; cpu.registers[29]=16;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::address_fault,
          "Invalid kernel callback stack was accepted");
    cpu.pc=intrman.module().entry+36;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::unsupported,
          "Inactive kernel return gate accepted a call");
}
void native_interrupt_dispatch() {
    nt::Bytes ram(0x10000);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman intrman(devices,0x4000); intrman.install(ram,libraries);
    nt::iop::CallRouter calls; calls.add(intrman);
    const auto invoke=[&](unsigned ordinal,std::array<std::uint32_t,4> args) {
        return intrman.invoke(nt::le32(intrman.module().memory,20+ordinal*4),args,ram,false);
    };
    check(invoke(4,{4,2,0x200,0x1234}).state==nt::iop::CallState::returned,"IRQ handler registration failed");
    invoke(6,{4,0,0,0}); invoke(9,{});
    fixture::put32(ram,0x200,0xac040700); // Record userdata.
    fixture::put32(ram,0x204,0x24020001); // Keep this source enabled.
    fixture::put32(ram,0x208,0x03e00008);
    fixture::put32(ram,0x104,0x24060077);
    nt::iop::Cpu cpu; cpu.pc=0x104; cpu.delay_slot=true; cpu.branch_target=0x180;
    cpu.status=0x401; cpu.registers[29]=0x9000; cpu.registers[2]=0xabcd;
    cpu.hi=0x123; cpu.lo=0x456; cpu.load_register=5; cpu.load_value=33;
    devices.interrupts().raise(1U<<4);
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x204 && cpu.interrupt_depth==1 && cpu.epc==0x100 && (cpu.cause&0x80000000) &&
          nt::le32(ram,0x700)==0x1234 && !(*devices.read(0x1f801074,4)&16) &&
          !(*devices.read(0x1f801070,4)&16),"IRQ entry did not preserve delay state, mask, ack or pass userdata");
    check(nt::iop::run(cpu,ram,4,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x104 && cpu.delay_slot && cpu.branch_target==0x180 && !cpu.interrupt_depth &&
          cpu.registers[2]==0xabcd && cpu.registers[5]==33 && cpu.registers[29]==0x9000 &&
          cpu.hi==0x123 && cpu.lo==0x456 && cpu.status==0x401 &&
          (*devices.read(0x1f801074,4)&16),"IRQ return lost interrupted context or failed to reenable source");
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x180 && cpu.registers[6]==0x77,"Interrupted branch continuation changed");
    fixture::put32(ram,0x204,0x24020000);
    devices.interrupts().raise(16);
    nt::iop::run(cpu,ram,5,&devices,&calls);
    check(!(*devices.read(0x1f801074,4)&16),"Zero IRQ return did not leave source masked");
    invoke(6,{5,0,0,0}); devices.interrupts().raise(32);
    const auto stopped=cpu;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::interrupt_pending &&
          cpu.pc==stopped.pc && cpu.executed==stopped.executed && (*devices.read(0x1f801070,4)&32),
          "Unregistered source fell through to a missing vector or was discarded");
    devices.write(0x1f801070,4,~32U);
    // An interrupt can request guest scheduler selection of a different thread.
    invoke(28,{0x400,0,0,0}); invoke(30,{0x300,0,0,0}); invoke(6,{4,0,0,0});
    fixture::put32(ram,0x300,0x24020001); fixture::put32(ram,0x304,0x03e00008);
    fixture::put32(ram,0x400,0x24023000); fixture::put32(ram,0x404,0x03e00008);
    fixture::put32(ram,0x3000,0xfffffffe); fixture::put32(ram,0x3010,0xbeef);
    fixture::put32(ram,0x3074,0xa000); fixture::put32(ram,0x3088,0x404);
    fixture::put32(ram,0x308c,0x500); fixture::put32(ram,0x3090,1);
    devices.interrupts().raise(16);
    check(nt::iop::run(cpu,ram,13,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x500 && cpu.registers[4]==0xbeef && cpu.registers[29]==0xa000 && !cpu.interrupt_depth,
          "Interrupt preemption did not execute guest scheduler or restore its selected thread");
    check(nt::le32(ram,0x9000-0x98)==0xfffffffe &&
          nt::le32(ram,0x9000-0x98+0x8c)==0x180,"Preemption did not save outgoing full context");
}
void thread_context_switch() {
    nt::Bytes ram(0x10000);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman intrman(devices,0x4000);
    intrman.install(ram,libraries);
    nt::iop::CallRouter calls; calls.add(intrman);
    const auto entry=nt::le32(intrman.module().memory,20+28*4);
    check(intrman.invoke(entry,{0x200,0,0,0},ram,false).state==nt::iop::CallState::returned,
          "Context callback registration failed");
    nt::iop::Dmacman dma(devices,0x6000); dma.install(ram,libraries); calls.add(dma);
    nt::iop::Cpu unsupported; unsupported.pc=dma.module().entry;
    unsupported.registers[2]=0x20; unsupported.registers[29]=0x9000;
    check(nt::iop::run(unsupported,ram,1,&devices,&calls).reason==nt::iop::CpuStop::unsupported &&
          unsupported.executed==0,"Unimplemented native service was reinterpreted as a thread yield");
    unsupported.pc=0x100; unsupported.registers[29]=16;
    fixture::put32(ram,0x100,12);
    check(nt::iop::run(unsupported,ram,1,&devices,&calls).reason==nt::iop::CpuStop::address_fault &&
          unsupported.executed==0,"Invalid outgoing stack changed CPU state");
    fixture::put32(ram,0x100,12); // Thread yield.
    fixture::put32(ram,0x200,0x00801021); // Return the outgoing context.
    fixture::put32(ram,0x204,0x24100055); // Clobber s0, which must be restored.
    fixture::put32(ram,0x208,0x03e00008);
    nt::iop::Cpu cpu; cpu.pc=0x100; cpu.registers[2]=0x20;
    cpu.registers[4]=17; cpu.registers[5]=23; cpu.registers[6]=1;
    cpu.registers[16]=0x1234; cpu.registers[29]=0x9000; cpu.registers[31]=0x300;
    cpu.status=0x401; cpu.load_register=17; cpu.load_value=0x5678;
    check(nt::iop::run(cpu,ram,1,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x200 && cpu.interrupt_depth==1 && cpu.load_register==0,
          "Yield did not enter the registered guest callback");
    const auto frame=0x9000-0x98;
    check(cpu.registers[4]==frame && nt::le32(ram,frame)==0xf0ff000c &&
          nt::le32(ram,frame+8)==17 && nt::le32(ram,frame+12)==23 &&
          nt::le32(ram,frame+17*4)==0x5678 && nt::le32(ram,frame+0x8c)==0x104,
          "Outgoing context ABI or delayed load differs");
    check(nt::iop::run(cpu,ram,5,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x104 && cpu.registers[2]==17 && cpu.registers[3]==23 &&
          cpu.registers[16]==0x1234 && cpu.registers[17]==0x5678 && cpu.registers[29]==0x9000 &&
          cpu.registers[31]==0x300 && cpu.status==0x401 && !cpu.interrupt_depth,
          "Voluntary context did not resume across split execution budgets");
    check(devices.read(0x1f801078,4)==1,"Restored context did not resume its interrupt gate");
    // A scheduler can select a distinct newly created full context.
    fixture::put32(ram,0x200,0x24023000);
    fixture::put32(ram,0x3000,0xfffffffe);
    for (unsigned reg=1;reg<32;++reg) fixture::put32(ram,0x3000+reg*4,0x1000+reg);
    fixture::put32(ram,0x3080,0xaabb); fixture::put32(ram,0x3084,0xccdd);
    fixture::put32(ram,0x3088,0x404); fixture::put32(ram,0x308c,0x500);
    fixture::put32(ram,0x3090,1);
    cpu.pc=0x100; cpu.registers[2]=0x20; cpu.registers[6]=0;
    check(nt::iop::run(cpu,ram,6,&devices,&calls).reason==nt::iop::CpuStop::budget &&
          cpu.pc==0x500 && cpu.registers[4]==0x1004 && cpu.registers[16]==0x1010 &&
          cpu.registers[29]==0x101d && cpu.hi==0xaabb && cpu.lo==0xccdd && cpu.status==0x401,
          "Scheduler-selected full context was not restored");
    // Invalid incoming frames must stop before restoring CPU or device state.
    fixture::put32(ram,0x308c,0xbfc00000);
    cpu.pc=0x100; cpu.registers[2]=0x20; cpu.registers[6]=0; cpu.registers[29]=0x9000;
    check(nt::iop::run(cpu,ram,5,&devices,&calls).reason==nt::iop::CpuStop::budget,
          "Callback failed before its return boundary");
    const auto before=cpu;
    const auto failure=nt::iop::run(cpu,ram,1,&devices,&calls);
    check(failure.reason==nt::iop::CpuStop::address_fault && failure.address==0xbfc00000 &&
          cpu.pc==before.pc && cpu.executed==before.executed && cpu.registers==before.registers,
          "Invalid context target did not fail atomically");
}
void resource_directory() {
    const nt::iop::Resources empty;
    const auto metadata=nt::disc::parse_reboot_image(empty.image());
    check(metadata.size()==3 && empty.image().size()==64 && metadata[0].size==0,
          "Default resource directory must contain only metadata and no bootstrap code");
    const std::vector<nt::iop::ResourceFile> files{{"EXAMPLE",{1,2,3,4,5}},{"EMPTY",{}}};
    const nt::iop::Resources resources(files);
    const auto entries=nt::disc::parse_reboot_image(resources.image());
    check(entries.size()==5 && entries[3].size==5 && entries[4].offset==112 &&
          resources.read(0x1fc00060,4)==0x04030201 && resources.read(0x1fc00064,1)==5,
          "Resource payload offsets, alignment or byte order differ");
    check(!resources.read(0x1fc00061,4) && !resources.read(0x1fc00070,1) &&
          !resources.read(0x1fbfffff,1) && !resources.read(0x1fc00000,8),
          "Resource mapping accepted an invalid access");
    for (const auto name : {"RESET","ROMDIR","EXTINFO","BAD/NAME","TOO-LONG-NAME",""}) {
        bool rejected=false;
        try { const nt::iop::Resources invalid(std::vector<nt::iop::ResourceFile>{{name,{}}}); }
        catch (const std::runtime_error&) { rejected=true; }
        check(rejected,"Invalid or reserved resource name accepted");
    }
    bool rejected=false;
    try { const nt::iop::Resources invalid(std::vector<nt::iop::ResourceFile>{{"DUP",{}},{"DUP",{}}}); }
    catch (const std::runtime_error&) { rejected=true; }
    check(rejected,"Duplicate resource name accepted");
    nt::iop::BootstrapDevices devices;
    nt::Bytes ram(256);
    fixture::put32(ram,0,0x8c820000); // lw v0,0(a0), followed by load-delay NOP.
    for (const auto address : {0x1fc00000U,0x9fc00000U,0xbfc00000U}) {
        nt::iop::Cpu cpu; cpu.registers[4]=address;
        check(nt::iop::run(cpu,ram,2,&devices).reason==nt::iop::CpuStop::budget &&
              cpu.registers[2]==0x45534552,"Guest could not read resource directory through an alias");
        cpu={}; cpu.pc=address;
        check(nt::iop::run(cpu,ram,1,&devices).reason==nt::iop::CpuStop::address_fault && cpu.executed==0,
              "Resource data was treated as executable firmware");
    }
    check(!devices.write(0x1fc00000,4,0) && devices.read(0x1fc00000,4)==0x45534552,
          "Resource directory is writable");
}
void startup_dependencies() {
    const auto table=[](std::string name,std::uint16_t version,unsigned count) {
        return nt::iop::ExportTable{std::move(name),version,std::vector<std::uint32_t>(count,0x2000)};
    };
    const std::vector<nt::iop::ModuleDependencies> modules{
        {"consumer",100,{{"service",0x101,4,0}},{}},
        {"replacement",200,{{"io",0x101,1,0}},{table("print",0x102,5)}},
        {"io",300,{{"service",0x101,2,0}},{table("io",0x101,3)}},
        {"service",400,{{"print",0x101,4,0}},{table("service",0x102,5)}},
        {"base",500,{}, {table("print",0x101,5)}},
        {"independent",600,{}, {}}
    };
    const auto plan=nt::iop::plan_startup(modules,{});
    check(plan.blocked.empty() && plan.file_offsets==std::vector<std::uint32_t>{500,400,100,300,200,600},
          "Startup order ignored readiness, compatible minor versions or stable input order");
    const std::vector<nt::iop::ModuleDependencies> cycle{
        {"first",10,{{"second",0x101,0,0}},{table("first",0x101,1)}},
        {"second",20,{{"first",0x101,0,0}},{table("second",0x101,1)}}
    };
    const auto blocked=nt::iop::plan_startup(cycle,{});
    check(blocked.file_offsets.empty() && blocked.blocked.size()==2 && !blocked.blocked[0].imports.empty(),
          "Dependency cycle was silently scheduled");
    const std::vector<nt::iop::ModuleDependencies> single{modules.front()};
    for (const auto incompatible : {table("service",0x100,5),table("service",0x201,5),table("service",0x102,4)})
        check(nt::iop::plan_startup(single,{incompatible}).blocked.size()==1,"Planner accepted incompatible ABI or missing ordinal");
    check(nt::iop::plan_startup(single,{table("service",0x101,5),table("service",0x102,4)}).blocked.size()==1,
          "Planner used a shadowed older provider to satisfy a missing ordinal");
}
void provider_registration() {
    nt::Bytes ram(2*1024*1024);
    nt::elf::IopModule provider{0x3000,0x3000,nt::Bytes(96),96,{}};
    fixture::put32(provider.memory,0,0xac040700); // Record the supplied export-table address.
    fixture::put32(provider.memory,4,0xafa0000c); // Callee may use all four argument-save slots.
    fixture::put32(provider.memory,8,0x03e00008);
    fixture::put32(provider.memory,12,0x24020000);
    fixture::put32(provider.memory,32,0x41c00000);
    fixture::put32(provider.memory,40,0x103);
    const std::string name="loadcore";
    std::copy(name.begin(),name.end(),provider.memory.begin()+44);
    for (unsigned i=0;i<7;++i) fixture::put32(provider.memory,52+i*4,provider.base);
    nt::iop::Libraries libraries;
    libraries.register_exports(provider,provider.base+32);
    std::copy(provider.memory.begin(),provider.memory.end(),ram.begin()+provider.base);
    const auto file=nt::iop::registration_module(0x2000,0x2200,0x103);
    for (const auto base : {0x4000U,0x14000U}) {
        fixture::put32(ram,0x2200,0);
        const auto adapter=nt::elf::load_iop_module(file,base);
        check(adapter.info && adapter.info->name=="AnyPS2_register", "Registration IRX metadata differs");
        nt::iop::Startup call(ram,adapter,libraries,0x8000,0x9000,nt::iop::EntryRegisters{});
        check(call.resume(5).reason==nt::iop::StartupStop::budget && nt::le32(ram,0x2200)==0,
              "Registration completion was reported before its guest call");
        check(call.resume(100).reason==nt::iop::StartupStop::returned && nt::le32(ram,0x700)==0x2000 &&
              nt::le32(ram,0x2200)==1,"Relocated registration module did not call the export and return");
    }
    fixture::put32(ram,provider.base+12,0x2402ffff);
    fixture::put32(ram,0x2200,0);
    nt::iop::Startup failing(ram,nt::elf::load_iop_module(file,0x4000),libraries,0x8000,0x9000,nt::iop::EntryRegisters{});
    const auto result=failing.resume(100);
    check(result.reason==nt::iop::StartupStop::fault && result.execution.instruction==13 &&
          nt::le32(ram,0x2200)==0,"Rejected guest registration was reported as complete");
}
nt::iop::StartupResult linked_service_call(nt::Bytes& ram,nt::iop::Libraries& libraries,
    nt::iop::BootstrapDevices& devices,nt::iop::HostCalls& calls,const std::string& name,
    unsigned ordinal,std::array<std::uint32_t,4> args,bool fault=false,std::uint16_t version=0x102) {
        nt::elf::IopModule consumer{0x1000,0x1000,nt::Bytes(68),68,{}};
        const std::uint32_t code[]{0x03e08021,0x0c00040d,0,0x02000008,0};
        for (unsigned i=0;i<std::size(code);++i) fixture::put32(consumer.memory,i*4,code[i]);
        fixture::put32(consumer.memory,32,0x41e00000); fixture::put32(consumer.memory,40,version);

        std::copy(name.begin(),name.end(),consumer.memory.begin()+44);
        fixture::put32(consumer.memory,52,0x03e00008);
        fixture::put32(consumer.memory,56,0x24000000|ordinal);
        nt::iop::Startup startup(ram,consumer,libraries,0x8000,0x9000,nt::iop::EntryRegisters{args},&devices,&calls);
        check(startup.resume(3).reason==nt::iop::StartupStop::budget,"Service call did not suspend at guest call boundary");
        const auto result=startup.resume(20);
        check(result.reason==(fault ? nt::iop::StartupStop::fault : nt::iop::StartupStop::returned),
              "Linked service call did not reach its expected return or fault boundary");
        return result;
}
void heaplib_service() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    std::map<std::uint32_t,std::uint32_t> backing;
    std::uint32_t next=0x10000;
    bool exhausted=false, release_fails=false;
    unsigned last_policy=0;
    nt::iop::Heaplib service(0x4000,[&](auto size,auto policy) {
        last_policy=policy;
        if (exhausted) return nt::iop::HeapRegion{};
        const auto base=next; next+=0x1000;
        const auto actual=(size+255)&~255U;
        backing.emplace(base,actual);
        return nt::iop::HeapRegion{base,actual};
    },[&](auto base) { return !release_fails && backing.erase(base)==1; });
    service.install(ram,libraries);
    const auto call=[&](unsigned ordinal,std::array<std::uint32_t,4> args={},bool fault=false) {
        return linked_service_call(ram,libraries,devices,service,"heaplib",ordinal,args,fault,0x101).return_value;
    };
    const auto heap=call(4,{128,0}), other=call(4,{128,0});
    check(heap && other && heap!=other && call(8,{heap})==88,"Heap creation or initial free capacity differs");
    const auto first=call(6,{heap,16}), second=call(6,{heap,8});
    check(first && second && first%8==0 && second>=first+16 && call(8,{heap})==48,
          "Heap suballocation alignment, extent or accounting differs");
    fixture::put32(ram,second,0xdeadbeef);
    check(call(7,{other,first})==std::uint32_t(-1) && call(7,{heap,first+8})==std::uint32_t(-1),
          "Heap accepted an interior pointer or another heap's allocation");
    check(call(7,{heap|0x80000000,first|0xa0000000})==0 && nt::le32(ram,second)==0xdeadbeef &&
          call(7,{heap,first})==std::uint32_t(-1),"Heap free alias, data preservation or double-free rejection differs");
    check(call(7,{heap,second})==0 && call(8,{heap})==88,"Adjacent freed blocks were not coalesced");
    const auto large=call(6,{heap,72});
    check(large && call(6,{heap,0})==0 && backing.size()==2,"Fixed heap grew or failed to reuse coalesced space");
    call(5,{heap}); call(5,{other});
    check(backing.empty() && call(8,{heap})==std::uint32_t(-4) && call(7,{heap,large})==std::uint32_t(-4),
          "Heap deletion retained backing or stale handles");
    const auto growing=call(4,{64,3});
    const auto small=call(6,{growing,8});
    const auto initial_free=call(8,{growing});
    const auto expanded=call(6,{growing,128});
    check(small && expanded && last_policy==1 && backing.size()==2,"Expandable heap did not request backing with its policy");
    check(call(7,{growing,expanded})==0 && backing.size()==1 && call(8,{growing})==initial_free,
          "Empty expansion block was not returned to the memory service");
    exhausted=true;
    check(call(6,{growing,128})==0 && call(4,{128,0})==0 && call(6,{growing,0xffffffff})==0 && backing.size()==1,
          "Heap exhaustion/overflow fabricated memory");
    release_fails=true; call(5,{growing},true);
    check(backing.size()==1 && call(8,{growing})==initial_free,"Rejected backing release destroyed heap state");
    release_fails=false; call(5,{growing});
    check(backing.empty(),"Heap deletion leaked an expansion or root block");
    call(4,{128,4},true); call(11,{},true);
}
void dmacman_service() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman intrman(devices,0x2000);
    nt::iop::Dmacman dmacman(devices,0x3000);
    intrman.install(ram,libraries); dmacman.install(ram,libraries);
    nt::iop::CallRouter calls; calls.add(intrman); calls.add(dmacman);
    const auto call=[&](unsigned ordinal,std::array<std::uint32_t,4> args={},bool fault=false) {
        return linked_service_call(ram,libraries,devices,calls,"dmacman",ordinal,args,fault);
    };
    call(4,{3,0x12340}); call(6,{3,0x100020}); call(8,{3,0x200});
    check(devices.read(0x1f8010b0,4)==0x12340 && devices.read(0x1f8010b4,4)==0x100020 &&
          devices.read(0x1f8010b8,4)==0x200,"Native DMA calls did not reach the primary register bank");
    for (const auto alias : {0x1f801520U,0x9f801520U,0xbf801520U}) {
        fixture::put32(ram,0x5000,0xad090000); fixture::put32(ram,0x5004,0x8d020000); fixture::put32(ram,0x5008,0);
        nt::iop::Cpu cpu; cpu.pc=0x5000; cpu.registers[8]=alias; cpu.registers[9]=0x45670;
        check(nt::iop::run(cpu,ram,3,&devices).reason==nt::iop::CpuStop::budget && cpu.registers[2]==0x45670 &&
              call(5,{9}).return_value==0x45670,"IOP DMA aliases and native reads diverged");
    }
    call(10,{9,0x6000});
    check(call(11,{9}).return_value==0x6000,"SIF0 DMA tag address was not retained");
    call(16,{0x12345678}); call(33,{9,5}); call(34,{9});
    check(call(17).return_value==0x12345d78,"DMA priority or enable changed another channel");
    call(35,{9}); check(call(17).return_value==0x12345578,"DMA disable changed priority bits");
    call(26,{1}); check(devices.read(0x1f801578,4)==1 && call(27).return_value==1,"DMA global enable storage diverged");
    check(call(8,{3,0x01000200},true).execution.reason==nt::iop::CpuStop::unsupported &&
          devices.read(0x1f8010b8,4)==0x200 && !devices.write(0x1f801528,4,0x01000000) &&
          devices.read(0x1f801528,4)==0,"Unsupported DMA start partially committed");
    call(10,{10,0x7000},true); call(4,{13,0},true); call(32,{3},true); call(20,{0},true);
    check(!devices.write(0x1f801520,1,0) && !devices.read(0x1f80153c,4) &&
          devices.read(0x1f801520,4)==0x45670,"Rejected DMA access changed channel state");
    call(6,{10,0xabcd1234});
    check(devices.write(0x1f801534,2,0x5678) && call(7,{10}).return_value==0xabcd5678 &&
          devices.write(0x1f801536,2,0x1234) && devices.read(0x1f801534,4)==0x12345678 &&
          devices.read(0x1f801536,2)==0x1234,"DMA halfword access lost the other register half");
    check(!devices.write(0x1f80153a,2,0x100) && devices.read(0x1f801538,4)==0,
          "Halfword DMA START bypassed transfer validation");
    check(linked_service_call(ram,libraries,devices,calls,"intrman",23,{}).return_value==0,
          "Provider routing displaced the interrupt service");
}
void intrman_service() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    nt::iop::Libraries libraries;
    nt::iop::Intrman service(devices,0x2000);
    service.install(ram,libraries);
    const auto call=[&](unsigned ordinal,std::array<std::uint32_t,4> args,bool fault=false) {
        return linked_service_call(ram,libraries,devices,service,"intrman",ordinal,args,fault);
    };
    check(call(4,{1,2,0x5000,0xabcdef}).return_value==0 && service.handler(1) &&
          service.handler(1)->entry==0x5000 && service.handler(1)->argument==0xabcdef && service.handler(1)->mode==2,
          "Linked intrman registration lost handler data");
    check(call(4,{1,0,0x6000,0}).return_value==std::uint32_t(-104) && service.handler(1)->entry==0x5000,
          "Duplicate registration replaced the existing handler");
    check(call(6,{1,0,0,0}).return_value==0 && call(9,{}).return_value==0,"Interrupt enable failed");
    devices.interrupts().raise(2);
    check(devices.interrupt_pending(),"Service masks diverged from controller state");
    check(call(17,{0xbf800001,0,0,0},true).execution.reason==nt::iop::CpuStop::address_fault && devices.interrupt_pending(),
          "Invalid suspend output changed the master gate");
    check(call(17,{0x80000600,0,0,0}).return_value==0 && nt::le32(ram,0x600)==1 && !devices.interrupt_pending(),
          "Suspend did not save the enabled state through a RAM alias");
    check(call(19,{0x604,0,0,0}).return_value==std::uint32_t(-102) && nt::le32(ram,0x604)==0,
          "Nested suspend did not preserve disabled state");
    check(call(20,{0,0,0,0}).return_value==0 && !devices.interrupt_pending() &&
          call(18,{1,0,0,0}).return_value==0 && devices.interrupt_pending(),"Nested resume enabled interrupts too early");
    check(call(7,{1,0x608,0,0}).return_value==0 && nt::le32(ram,0x608)==1 && !devices.interrupt_pending(),
          "DisableIntr did not return the prior interrupt number");
    check(call(7,{1,0x608,0,0}).return_value==std::uint32_t(-103) && nt::le32(ram,0x608)==std::uint32_t(-103),
          "Already-disabled source reported success");
    check(call(5,{1,0,0,0}).return_value==0 && !service.handler(1) &&
          call(5,{1,0,0,0}).return_value==std::uint32_t(-105),"Handler release lifecycle differs");
    check(call(6,{39,0,0,0}).return_value==std::uint32_t(-101) &&
          call(15,{},true).execution.reason==nt::iop::CpuStop::unsupported,"Unsupported service entry fabricated success");
    for (const auto irq : {32U,42U,63U}) {
        check(call(4,{irq,1,0x5000,0x1234}).return_value==0 &&
              service.handler(irq)->entry==0x5000 && service.handler(irq)->argument==0x1234 &&
              call(4,{irq,1,0x6000,0}).return_value==std::uint32_t(-104),
              "DMA handler registration lost state or accepted a duplicate");
        check(call(5,{irq,0,0,0}).return_value==0 && !service.handler(irq) &&
              call(5,{irq,0,0,0}).return_value==std::uint32_t(-105),"DMA handler release differs");
    }
    check(call(4,{64,1,0x5000,0}).return_value==std::uint32_t(-101),"Invalid interrupt number accepted");
    for (unsigned channel=0;channel<13;++channel) {
        const bool second=channel>=7;
        const auto irq=second ? channel+33 : channel+32;
        const auto index=second ? channel-7 : channel;
        const auto address=second ? 0x1f801574U : 0x1f8010f4U;
        const auto flag=1U<<(24+index), enable=1U<<(16+index);
        check(call(6,{irq|0x200,0,0,0}).return_value==0 &&
              (*devices.read(address,4)&enable),"DMA source did not enable through intrman");
        devices.write(0x1f801078,4,1);
        check(devices.signal_dma_completion(channel) && devices.interrupt_pending() &&
              (*devices.read(address,4)&flag),"DMA completion did not latch and route");
        // Disable must preserve pending flags and return the complete mode value.
        check(call(7,{irq,0x608,0,0}).return_value==0 && nt::le32(ram,0x608)==(irq|0x200) &&
              (*devices.read(address,4)&flag),"DMA disable lost flags or prior mode");
        const auto value=*devices.read(address,4)&0xffffff;
        devices.write(address,4,value|flag);
        check(!(*devices.read(address,4)&flag),"DICR acknowledgement did not clear selected flag");
        devices.write(0x1f801070,4,~8U);
        check(!devices.interrupt_pending(),"DMA acknowledgement left controller pending");
        check(call(7,{irq,0x608,0,0}).return_value==std::uint32_t(-103),"Disabled DMA source reported enabled");
    }
    const auto prior=*devices.read(0x1f801574,4);
    check(call(7,{42,3,0,0},true).execution.address==3 && devices.read(0x1f801574,4)==prior,
          "Invalid DMA output pointer mutated control state");
    check(!devices.signal_dma_completion(13) && !devices.write(0x1f801574,2,0),
          "Invalid DMA channel or width accepted");
    devices.write(0x1f8010f4,4,0x8000);
    check(devices.interrupt_pending() && (*devices.read(0x1f8010f4,4)&0x80000000U),"Forced DMA interrupt not routed");
    devices.write(0x1f8010f4,4,0); devices.write(0x1f801070,4,~8U);
    call(28,{0x5000,0,0,0}); call(30,{0x80006000,0,0,0});
    check(service.context_callback()==0x5000 && service.preemption_callback()==0x80006000,
          "Thread callback registration lost the supplied guest entries");
    check(call(28,{3,0,0,0},true).execution.address==3 && service.context_callback()==0x5000,
          "Rejected thread callback replaced the registered entry");
    call(29,{}); call(31,{});
    check(!service.context_callback() && !service.preemption_callback(),"Thread callback reset did not clear registrations");
    check(call(23,{}).return_value==0,"Normal service call reported interrupt context");
    const auto query=*libraries.resolve({"intrman",0x102,23,0});
    nt::iop::Cpu cpu; cpu.pc=query; cpu.interrupt_depth=1;
    nt::iop::run(cpu,ram,1,&devices,&service);
    check(cpu.registers[2]==1,"Guest bridge lost interrupt context");
    cpu.pc=*libraries.resolve({"intrman",0x102,4,0}); cpu.registers[4]=1; cpu.registers[6]=0x5000;
    nt::iop::run(cpu,ram,1,&devices,&service);
    check(cpu.registers[2]==std::uint32_t(-100) && !service.handler(1),"Interrupt-context registration changed handlers");
    fixture::put32(ram,0x7000,12); cpu={}; cpu.pc=0x7000;
    check(nt::iop::run(cpu,ram,1,&devices,&service).reason==nt::iop::CpuStop::unsupported && cpu.executed==0,
          "Ordinary guest syscall was intercepted as a host service");
}
void archive_dependencies() {
    auto image=fixture::manager_bootstrap_image();
    const auto original=image;
    check(nt::iop::inspect_dependencies(image).missing_libraries.empty() && image==original,
          "Archive inventory missed an available export or mutated input");
    const std::string absent="missing";
    std::copy(absent.begin(),absent.end(),image.begin()+480+172);
    const auto report=nt::iop::inspect_dependencies(image);
    check(report.missing_libraries.size()==1 && report.missing_libraries[0].module=="LOADCORE" &&
          report.missing_libraries[0].imports.size()==1 &&
          report.missing_libraries[0].imports[0].library=="missing" &&
          report.missing_libraries[0].imports[0].version==0x102 &&
          report.missing_libraries[0].imports[0].ordinal==4,
          "Archive inventory lost the unresolved consumer or ABI requirement");
    check(nt::iop::inspect_dependencies(fixture::reboot_image()).missing_libraries.empty(),
          "Archive metadata was treated as executable code");
}
void delays() {
    nt::Bytes ram(2*1024*1024);
    const std::uint32_t program[]{0x8c820000,0x00401821,0x00402821,0x10a60004,
        0x24070007,0x24070063,0,0,0xac870004,0x0800000c,0x24080008,0x24080063,0xffffffff};
    for (unsigned i=0;i<std::size(program);++i) fixture::put32(ram,i*4,program[i]);
    fixture::put32(ram,0x100,42);
    nt::iop::Cpu cpu; cpu.registers[2]=9; cpu.registers[4]=0x100; cpu.registers[6]=42;
    const auto initial=cpu;
    auto result=nt::iop::run(cpu,ram,4);
    check(result.reason==nt::iop::CpuStop::budget && cpu.pc==16 && cpu.delay_slot &&
          cpu.registers[3]==9 && cpu.registers[5]==42,"Load delay or branch budget boundary differs");
    result=nt::iop::run(cpu,ram,20);
    check(result.reason==nt::iop::CpuStop::unsupported && cpu.pc==48 && cpu.executed==8 &&
          cpu.registers[7]==7 && cpu.registers[8]==8 && nt::le32(ram,0x104)==7,
          "Taken branch, jump delay or store differs");
    auto uninterrupted=initial;
    result=nt::iop::run(uninterrupted,ram,24);
    check(uninterrupted.registers==cpu.registers && uninterrupted.executed==cpu.executed &&
          uninterrupted.pc==cpu.pc,"Budget suspension changed execution");
    // A not-taken branch still executes its delay slot.
    auto not_taken=initial; not_taken.registers[6]=43;
    nt::iop::run(not_taken,ram,6);
    check(not_taken.pc==24 && not_taken.registers[7]==99,"Not-taken branch skipped fallthrough");
    fixture::put32(ram,4,0x24020011);
    cpu=initial; nt::iop::run(cpu,ram,3);
    check(cpu.registers[2]==17 && cpu.registers[5]==17,"Direct write did not cancel delayed load");
    fixture::put32(ram,4,0x8c820004); fixture::put32(ram,0x104,77);
    cpu=initial; nt::iop::run(cpu,ram,3);
    check(cpu.registers[5]==9 && cpu.registers[2]==77,"Consecutive loads to the same register differ");
    cpu=initial; cpu.pc=0x80000000; nt::iop::run(cpu,ram,1);
    check(cpu.load_value==42 && cpu.pc==0x80000004,"Cached RAM alias differs");
    for (const auto instruction : {0xac820000U,0x20820001U,0xffffffffU}) {
        fixture::put32(ram,0,instruction); cpu=initial; cpu.registers[4]=0x7fffffff;
        const auto before=cpu; const auto memory=ram;
        result=nt::iop::run(cpu,ram,1);
        check(result.reason!=nt::iop::CpuStop::budget && cpu.registers==before.registers &&
              cpu.executed==0 && cpu.pc==0 && memory==ram,"Fault partially committed state");
        if (instruction==0xac820000) check(result.address==0x7fffffff,"Fault omitted effective address");
    }
    fixture::put32(ram,0,0x08000008); fixture::put32(ram,4,0x08000008);
    cpu={}; result=nt::iop::run(cpu,ram,2);
    check(result.reason==nt::iop::CpuStop::branch_in_delay && cpu.pc==4 && cpu.executed==1,
          "Branch in delay slot did not stop precisely");
}
void imported_call() {
    nt::elf::IopModule provider{0x2000,0x2000,nt::Bytes(64),64,{}};
    fixture::put32(provider.memory,0,0x24820001); // addiu v0,a0,1
    fixture::put32(provider.memory,4,0x03e00008); // jr ra
    fixture::put32(provider.memory,32,0x41c00000);
    provider.memory[40]=1;
    const std::string name="sample";
    std::copy(name.begin(),name.end(),provider.memory.begin()+44);
    fixture::put32(provider.memory,52,0x2000);
    nt::elf::IopModule consumer{0x1000,0x1000,nt::Bytes(64),64,{}};
    fixture::put32(consumer.memory,0,0x0c00040d); // jal import stub at 0x1034
    fixture::put32(consumer.memory,4,0x24040029); // argument in delay slot
    fixture::put32(consumer.memory,8,0xffffffff);
    fixture::put32(consumer.memory,32,0x41e00000);
    consumer.memory[40]=1;
    std::copy(name.begin(),name.end(),consumer.memory.begin()+44);
    fixture::put32(consumer.memory,52,0x03e00008);
    fixture::put32(consumer.memory,56,0x24000000);
    // Import terminator requires a complete pair of zero words.
    consumer.memory.resize(68); consumer.initialized_size=68;
    nt::iop::Libraries libraries; libraries.register_exports(provider,0x2020); libraries.bind(consumer);
    nt::Bytes ram(2*1024*1024);
    std::copy(provider.memory.begin(),provider.memory.end(),ram.begin()+provider.base);
    std::copy(consumer.memory.begin(),consumer.memory.end(),ram.begin()+consumer.base);
    nt::iop::Cpu cpu; cpu.pc=consumer.entry;
    const auto result=nt::iop::run(cpu,ram,30);
    check(result.reason==nt::iop::CpuStop::unsupported && cpu.pc==0x1008 && cpu.executed==7 &&
          cpu.registers[2]==42 && cpu.registers[31]==0x1008 && cpu.registers[0]==0,
          "Registered import did not execute and return through its delay slots");
}
void status_register() {
    nt::Bytes ram(2*1024*1024);
    fixture::put32(ram,0,0x40886000); // mtc0 t0,status
    fixture::put32(ram,4,0x40026000); // mfc0 v0,status
    fixture::put32(ram,8,0x00401821);
    fixture::put32(ram,12,0x00402021);
    nt::iop::Cpu cpu; cpu.registers[8]=0xff01; cpu.registers[2]=7;
    nt::iop::run(cpu,ram,4);
    check(cpu.status==0xff01 && cpu.registers[3]==7 && cpu.registers[4]==0xff01,
          "IOP status transfer or coprocessor load delay differs");
    for (const auto unsupported : {2U,0x20000U}) {
        cpu={}; cpu.status=0xff01; cpu.registers[8]=unsupported;
        const auto result=nt::iop::run(cpu,ram,1);
        check(result.reason==nt::iop::CpuStop::unsupported && cpu.pc==0 && cpu.status==0xff01,
              "Unsupported IOP status mode changed state");
    }
    fixture::put32(ram,0,0x40027800); // mfc0 v0,PRId
    fixture::put32(ram,4,0x00401821); fixture::put32(ram,8,0x00402021);
    cpu={}; cpu.registers[2]=7;
    nt::iop::run(cpu,ram,3);
    check(cpu.registers[3]==7 && cpu.registers[4]==0x1f,"IOP processor revision read or delay differs");
    fixture::put32(ram,12,0x40827800);
    check(nt::iop::run(cpu,ram,1).reason==nt::iop::CpuStop::unsupported,"Read-only processor revision accepted a write");
}
void unaligned_words() {
    const auto encode=[](unsigned op,unsigned offset,unsigned target=2) {
        return (op<<26)|(4U<<21)|(target<<16)|offset;
    };
    for (const auto base : {0x100U,0x80000100U,0xa0000100U})
        for (const auto op : {34U,38U,42U,46U}) for (unsigned offset=0;offset<4;++offset) {
            nt::Bytes ram(1024,0);
            for (unsigned i=0;i<4;++i) ram[0x100+i]=static_cast<std::uint8_t>(0x11+i*0x11);
            fixture::put32(ram,0,encode(op,offset));
            nt::iop::Cpu cpu; cpu.registers[4]=base; cpu.registers[2]=0xddccbbaa;
            const bool load=op==34 || op==38, left=op==34 || op==42;
            std::array<std::uint8_t,4> expected=load ? std::array<std::uint8_t,4>{0xaa,0xbb,0xcc,0xdd} :
                                                                  std::array<std::uint8_t,4>{0x11,0x22,0x33,0x44};
            const std::array<std::uint8_t,4> source=load ? std::array<std::uint8_t,4>{0x11,0x22,0x33,0x44} :
                                                                    std::array<std::uint8_t,4>{0xaa,0xbb,0xcc,0xdd};
            for (unsigned i=0;i<(left ? offset+1 : 4-offset);++i) {
                const auto memory_byte=left ? i : offset+i, register_byte=left ? 3-offset+i : i;
                expected[load ? register_byte : memory_byte]=source[load ? memory_byte : register_byte];
            }
            check(nt::iop::run(cpu,ram,1).reason==nt::iop::CpuStop::budget &&
                  cpu.registers[2]==0xddccbbaa,"Partial access lost register state or load delay");
            nt::iop::run(cpu,ram,1);
            for (unsigned i=0;i<4;++i)
                check((load ? static_cast<std::uint8_t>(cpu.registers[2]>>(8*i)) : ram[0x100+i])==expected[i],
                      "Partial word byte selection differs");
        }
    for (const bool reverse : {false,true}) {
        nt::Bytes ram(1024,0);
        for (unsigned i=0;i<8;++i) ram[0x100+i]=static_cast<std::uint8_t>(0x10+i);
        fixture::put32(ram,0,encode(reverse ? 38 : 34,reverse ? 0 : 3));
        fixture::put32(ram,4,encode(reverse ? 34 : 38,reverse ? 3 : 0));
        fixture::put32(ram,8,0x00401821); fixture::put32(ram,12,0x00402821);
        nt::iop::Cpu cpu; cpu.registers[4]=0x101; cpu.registers[2]=0xabcdef01;
        const auto initial=cpu;
        nt::iop::run(cpu,ram,1); nt::iop::run(cpu,ram,1); nt::iop::run(cpu,ram,2);
        check(cpu.registers[2]==0x14131211 && cpu.registers[3]==0xabcdef01 && cpu.registers[5]==0x14131211,
              "Paired partial loads lost forwarding or exposed a result too early");
        auto whole=initial; nt::iop::run(whole,ram,4);
        check(whole.registers==cpu.registers && whole.load_register==cpu.load_register,
              "Partial loads changed across execution budgets");
    }
    nt::Bytes ram(1024,0);
    nt::iop::BootstrapDevices devices;
    devices.write(0x1f801078,4,1);
    for (const auto op : {34U,38U,42U,46U}) {
        fixture::put32(ram,0,encode(op,1,0));
        nt::iop::Cpu cpu; cpu.registers[4]=0xbf801078;
        const auto before=ram;
        check(nt::iop::run(cpu,ram,1,&devices).reason==nt::iop::CpuStop::address_fault &&
              cpu.pc==0 && cpu.executed==0 && ram==before,
              "Unsupported partial MMIO access mutated state");
    }
    check(devices.read(0x1f801078,4)==1,"Rejected partial MMIO read suspended interrupts");
    fixture::put32(ram,0,encode(42,1));
    nt::iop::Cpu isolated; isolated.status=0x10000; isolated.registers[4]=0x100; isolated.registers[2]=0xffffffff;
    nt::iop::run(isolated,ram,1);
    check(nt::le32(ram,0x100)==0,"Isolated partial store corrupted main RAM");
}
void cdvd_initialization() {
    using namespace std::chrono;
    static system_clock::time_point now=sys_days{year{2024}/February/28}+hours{15}+minutes{4}+seconds{59};
    nt::iop::Cdvd clock_device(+[] { return now; });
    check(clock_device.write(0x1f402016,1,8),"RTC command rejected");
    now+=hours{24}; // A pending response must remain latched.
    const std::array<unsigned,8> expected{0,0x59,0x04,0x00,0,0x29,0x02,0x24};
    for (const auto byte : expected) {
        check(clock_device.read(0x1f402017,1)==0 && clock_device.read(0x1f402018,1)==byte,
              "RTC BCD layout, leap-day rollover or latching differs");
    }
    check(clock_device.read(0x1f402017,1)==0x40,"RTC result did not drain");
    clock_device.write(0x1f402016,1,8);
    for (unsigned i=0;i<5;++i) clock_device.read(0x1f402018,1);
    check(clock_device.read(0x1f402018,1)==1 && clock_device.read(0x1f402018,1)==3,
          "RTC did not advance across leap-day boundary");
    nt::iop::BootstrapDevices devices;
    nt::Bytes ram(1024,0);
    fixture::put32(ram,0,0x91020008); // Original LBU through an IOP device alias.
    nt::iop::Cpu cpu; cpu.registers[8]=0xbf402000;
    check(nt::iop::run(cpu,ram,2,&devices).reason==nt::iop::CpuStop::budget && cpu.registers[2]==0,
          "CD/DVD interrupt register could not be read by guest code");
    check(devices.read(0x1f402005,1)==0x40 && devices.read(0x1f402017,1)==0x40 &&
          devices.read(0x1f40200a,1)==0,"CD/DVD initial controller state differs");
    devices.write(0x1f801074,4,4); devices.write(0x1f801078,4,1);
    check(devices.write(0x1f402004,1,0) && devices.read(0x1f402008,1)==2 &&
          devices.interrupt_pending(),"CD/DVD NOP did not signal command completion");
    devices.write(0x1f402008,1,1);
    check(devices.read(0x1f402008,1)==2,"CD/DVD acknowledgement cleared an unrelated cause");
    devices.write(0x1f402008,1,2); devices.write(0x1f801070,4,~4U);
    check(!devices.interrupt_pending(),"CD/DVD interrupt acknowledgement failed");
    devices.write(0x1f402005,1,7); devices.write(0x1f402004,1,0);
    check(devices.read(0x1f402006,1)==0x22 && devices.read(0x1f402006,1)==0 &&
          devices.read(0x1f402005,1)==0x41,"CD/DVD parameter error did not retain status or clear on read");
    devices.write(0x1f402004,1,0);
    check(devices.read(0x1f402005,1)==0x40,"CD/DVD NOP did not clear prior command error");
    devices.write(0x1f402017,1,0x30);
    check(!devices.write(0x1f402016,1,0xff) && devices.read(0x1f402016,1)==0 &&
          devices.write(0x1f402016,1,3),"Unsupported subcommand consumed pending parameters");
    check(devices.read(0x1f402017,1)==0 && devices.read(0x1f402018,1)==0 &&
          devices.read(0x1f402017,1)==0 && devices.read(0x1f402018,1)==0 &&
          devices.read(0x1f402017,1)==0x40 && devices.read(0x1f402018,1)==0,
          "CD/DVD result FIFO ready/empty transition differs");
    check(devices.write(0x1f402016,1,5) && devices.read(0x1f402018,1)==0,
          "Tray-state acknowledgement failed");
    check(!devices.write(0x1f402016,1,3),"Mechanical query accepted missing parameters");
    devices.write(0x1f402017,1,0);
    check(devices.write(0x1f402016,1,3),"Mechanical compatibility query rejected");
    for (const auto byte : {3U,6U,2U,0U})
        check(devices.read(0x1f402017,1)==0 && devices.read(0x1f402018,1)==byte,
              "Mechanical profile byte order or FIFO length differs");
    check(devices.read(0x1f402017,1)==0x40,"Mechanical query response did not terminate");
    check(devices.write(0x1f402016,1,0x1b) && devices.read(0x1f402018,1)==0 &&
          devices.read(0x1f402017,1)==0x40 && devices.read(0x1f402005,1)==0x40,
          "Power-off readiness cancellation changed unrelated controller state");
    for (unsigned i=0;i<16;++i) check(devices.write(0x1f402017,1,i),"CD/DVD parameter FIFO rejected capacity");
    check(!devices.write(0x1f402017,1,16) && !devices.write(0x1f402004,1,6) &&
          !devices.write(0x1f40203a,1,1) && !devices.read(0x1f402008,4) &&
          !devices.read(0x1f40200f,1),"Unsupported CD/DVD state was fabricated");
}
void sif1_receive() {
    nt::iop::BootstrapDevices devices;
    devices.enable_sif1_receive();
    nt::Bytes ram(1024,0xcc);
    std::deque<std::uint32_t> fifo{0x100,8,0,0};
    check(!devices.write(0x1f801538,4,0x41000301) && devices.read(0x1f801538,4)==0,
          "Unsupported SIF1 direction latched START");
    check(devices.write(0x1f801538,4,0x41000300),"Connected destination chain did not start");
    check(!devices.receive_sif1(fifo,ram) && fifo.size()==4,"Disabled DMA priority consumed a tag");
    devices.write(0x1f801570,4,0x8000);
    check(!devices.receive_sif1(fifo,ram) && fifo.empty() && devices.read(0x1f801530,4)==0x100,
          "SIF1 tag did not set destination or wait for payload");
    fifo={1,2,3,4};
    check(!devices.receive_sif1(fifo,ram) && devices.read(0x1f801530,4)==0x110 &&
          nt::le32(ram,0x100)==1 && nt::le32(ram,0x10c)==4,"SIF1 partial payload differs");
    check(!devices.write(0x1f801530,4,0x300) && !devices.write(0x1f801534,2,0),
          "Active DMA registers were overwritten");
    devices.write(0x1f801538,4,0);
    fifo={5,6,7,8};
    check(!devices.receive_sif1(fifo,ram) && fifo.size()==4,"Stopped DMA consumed data");
    devices.write(0x1f801538,4,0x41000300);
    check(devices.receive_sif1(fifo,ram) && fifo.empty() && nt::le32(ram,0x11c)==8 &&
          (*devices.read(0x1f801538,4)&0x1000000) && devices.read(0x1f801070,4)==0,
          "Nonterminal packet ended the chain or raised an interrupt");
    devices.write(0x1f801074,4,8); devices.write(0x1f801078,4,1);
    fifo={0x80000200,4,0,0,0x11223344,0x55667788,9,10};
    check(!devices.receive_sif1(fifo,ram) && fifo.size()==4 && devices.receive_sif1(fifo,ram) &&
          devices.read(0x1f801530,4)==0x210 && !(*devices.read(0x1f801538,4)&0x1000000) &&
          (*devices.read(0x1f801574,4)&0x08000000) && devices.interrupt_pending() &&
          nt::le32(ram,0x200)==0x11223344 && ram[0x210]==0xcc,
          "Terminating SIF1 packet lost payload, bounds or completion IRQ");
    devices.write(0x1f801574,4,0x08000000); devices.write(0x1f801070,4,~8U);
    check(!devices.interrupt_pending(),"SIF1 completion could not be acknowledged");
    devices.write(0x1f801538,4,0x41000300);
    fifo={0xc00003f8,4,0,0};
    const auto before=ram; const auto original=fifo;
    bool rejected=false;
    try { devices.receive_sif1(fifo,ram); } catch (const std::exception&) { rejected=true; }
    check(rejected && fifo==original && ram==before && devices.read(0x1f801530,4)==0x210,
          "Invalid SIF1 extent consumed a tag or changed memory");
    fifo={0x40000300,0,0,0};
    check(devices.receive_sif1(fifo,ram) && fifo.empty() && devices.interrupt_pending(),
          "Empty terminating SIF1 packet failed to complete");
}
void timer_interrupts() {
    constexpr std::uint32_t bases[]{0x1f801100,0x1f801110,0x1f801120,0x1f801480,0x1f801490,0x1f8014a0};
    constexpr unsigned irqs[]{4,5,6,14,15,16};
    for (unsigned i=0;i<6;++i) {
        nt::iop::BootstrapDevices devices;
        const auto base=bases[i], cause=1U<<irqs[i];
        devices.write(base+8,4,2);
        devices.write(base+4,2,0x58); // Repeated pulse, target reset.
        devices.instruction_retired(); devices.instruction_retired();
        check(devices.read(base,4)==0 && devices.read(0x1f801070,4)==cause &&
              !devices.interrupt_pending(),"Timer target failed to latch a masked IRQ");
        devices.write(0x1f801074,4,cause); devices.write(0x1f801078,4,1);
        check(devices.interrupt_pending(),"Timer cause did not reach the controller gate");
        devices.write(0x1f801070,4,~cause);
        devices.instruction_retired(); devices.instruction_retired();
        check(!devices.interrupt_pending(),"Pulse repeated before mode-read rearm");
        check(devices.read(base+4,4)==0x858,"Timer pulse flags differ");
        devices.instruction_retired(); devices.instruction_retired();
        check(devices.interrupt_pending(),"Mode read failed to rearm timer IRQ");
        devices.write(0x1f801070,4,0);
        devices.read(base+4,4);
        devices.write(base+4,4,0xd8); // Repeated toggle.
        for (unsigned event=0;event<4;++event) {
            devices.instruction_retired(); devices.instruction_retired();
            check(devices.interrupt_pending()==(event%2==0),"Timer toggle polarity differs");
            devices.write(0x1f801070,4,0);
        }
        devices.read(base+4,4);
        devices.write(base+4,4,0x20); // One-shot overflow.
        for (unsigned event=0;event<2;++event) {
            devices.write(base,4,i<3 ? 0xffffU : 0xffffffffU);
            devices.instruction_retired();
            check(devices.interrupt_pending()==(event==0),"One-shot overflow repeated without rearm");
            devices.write(0x1f801070,4,0);
        }
    }
    // A real timer event enters the existing guest exception path and returns.
    nt::iop::BootstrapDevices devices;
    nt::Bytes ram(1024,0);
    const std::uint32_t handler[]{0x401a7000,0,0xad001070,0x03400008,0x42000010};
    for (unsigned i=0;i<std::size(handler);++i) fixture::put32(ram,0x80+4*i,handler[i]);
    devices.write(0x1f8014a8,4,2); devices.write(0x1f8014a4,2,0x10);
    devices.write(0x1f801074,4,1U<<16); devices.write(0x1f801078,4,1);
    nt::iop::Cpu cpu; cpu.pc=0x200; cpu.status=0x401; cpu.registers[8]=0xbf800000;
    nt::iop::run(cpu,ram,2,&devices);
    check(cpu.pc==0x208 && devices.interrupt_pending(),"Timer did not assert after retirement");
    nt::iop::run(cpu,ram,5,&devices);
    check(cpu.epc==0x208 && cpu.pc==0x208 && cpu.status==0x401 &&
          !devices.interrupt_pending(),"Timer interrupt failed guest acknowledgement/RFE");
}
void timer_initialization() {
    nt::iop::BootstrapDevices devices;
    nt::Bytes ram(1024,0);
    nt::iop::Cpu cpu;
    // Original synthetic SH program: initialize timer 5 through its KSEG1 alias.
    fixture::put32(ram,0,0xa4850004);
    cpu.registers[4]=0xbf8014a0; cpu.registers[5]=0x2000;
    check(nt::iop::run(cpu,ram,7,&devices).reason==nt::iop::CpuStop::budget &&
          devices.read(0x1f8014a0,4)==0,"Timer initialization or prescaler phase differs");
    check(nt::iop::run(cpu,ram,1,&devices).reason==nt::iop::CpuStop::budget &&
          devices.read(0x1f8014a0,4)==1,"Timer did not advance across execution budgets");
    for (const auto base : {0x1f801100U,0x1f801110U,0x1f801120U,0x1f801480U,0x1f801490U,0x1f8014a0U}) {
        const bool wide=base>=0x1f801480;
        check(devices.write(base+4,2,0) && devices.write(base+8,4,2) &&
              devices.write(base,4,wide ? 0xffffffffU : 0xffffU),"Timer register setup failed");
        devices.instruction_retired();
        check(devices.read(base,4)==0 && devices.read(base+4,2)==0x1400 &&
              devices.read(base+4,4)==0x400,"Timer overflow or read-clear flags differ");
        devices.instruction_retired(); devices.instruction_retired();
        check(devices.read(base,4)==2 && devices.read(base+4,4)==0xc00 &&
              devices.read(base+4,4)==0x400,"Timer target polling differs");
        check(!devices.write(base+4,1,0) && !devices.write(base+5,2,0) &&
              !devices.write(base,1,0) &&
              devices.read(base,4)==2,"Unsupported timer writes mutated state");
    }
    check(devices.write(0x1f8014a4,2,1),"Timer stop mode rejected");
    for (unsigned i=0;i<16;++i) devices.instruction_retired();
    check(devices.read(0x1f8014a0,4)==0 && !devices.write(0x1f801104,2,0x100),
          "Stopped timer advanced or external clock accepted");
    check(devices.write(0x1f801480,4,0x12345678) && devices.read(0x1f801482,2)==0x1234 &&
          !devices.read(0x1f801482,4),"Wide timer halfword read differs");
    const auto before=devices.read(0x1f801480,4);
    fixture::put32(ram,cpu.pc,0xffffffff);
    check(nt::iop::run(cpu,ram,1,&devices).reason==nt::iop::CpuStop::unsupported &&
          devices.read(0x1f801480,4)==before,"Faulting instruction advanced timers");
}
void device_access() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    check(devices.read(0x1f801450,4)==0,"IOP device reset mode differs");
    check(devices.read(0x1f801578,4)==0,"IOP DMA control reset differs");
    for (const auto value : {1U,0U,0x12345678U}) {
        check(devices.write(0x1f801578,4,value) && devices.read(0x1f801578,4)==value,
              "IOP DMA control did not retain enable/disable state");
        check(!devices.write(0x1f801578,2,0) && !devices.read(0x1f801578,2) &&
              devices.read(0x1f801578,4)==value,"Rejected DMA width changed control state");
    }
    const std::uint32_t program[]{0xad091450,0x8d021450,0x00401821,0x00402021};
    for (unsigned i=0;i<std::size(program);++i) fixture::put32(ram,i*4,program[i]);
    for (const auto alias : {0x1f800000U,0x9f800000U,0xbf800000U}) {
        nt::iop::Cpu cpu; cpu.registers[8]=alias; cpu.registers[9]=0x100; cpu.registers[2]=7;
        const auto result=nt::iop::run(cpu,ram,4,&devices);
        check(result.reason==nt::iop::CpuStop::budget && cpu.registers[3]==7 && cpu.registers[4]==0x100 &&
              devices.read(0x1f801450,4)==0x100,"IOP device aliases or read delay differ");
    }
    for (const auto instruction : {0xad091450U,0xa1091450U,0x8d021454U}) {
        fixture::put32(ram,0,instruction);
        nt::iop::Cpu cpu; cpu.registers[8]=0xbf800000; cpu.registers[9]=8;
        const auto result=nt::iop::run(cpu,ram,1,&devices);
        check(result.reason==nt::iop::CpuStop::address_fault && result.address && cpu.executed==0 &&
              devices.read(0x1f801450,4)==0x100,"Rejected device access partially committed");
    }
}
void interrupt_delivery() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    auto& controller=devices.interrupts();
    controller.raise(0x12);
    check(!devices.interrupt_pending() && devices.read(0x1f801070,4)==0x12,
          "Masked IOP causes were lost or delivered");
    devices.write(0x1f801074,4,2); devices.write(0x1f801078,4,1);
    check(devices.interrupt_pending(),"IOP mask and master gate did not assert IP2");
    check(!devices.read(0x1f801078,2) && !devices.write(0x1f801078,4,2) && !devices.write(0x1f801070,1,0) &&
          devices.interrupt_pending(),"Rejected interrupt access mutated state");
    check(devices.read(0x1f801078,4)==1 && !devices.interrupt_pending() &&
          devices.read(0x1f801070,4)==0x12,"ICTRL read did not suspend while retaining causes");
    devices.write(0x1f801078,4,1);
    devices.write(0x1f801070,4,~2U);
    check(!devices.interrupt_pending() && devices.read(0x1f801070,4)==0x10,
          "IOP acknowledgement removed an unrelated cause");
    // Original exception handler: inspect EPC/Cause, acknowledge and return.
    const std::uint32_t handler[]{0x401a7000,0x401b6800,0,0xad001070,0x03400008,0x42000010};
    for (unsigned i=0;i<std::size(handler);++i) fixture::put32(ram,0x80+i*4,handler[i]);
    fixture::put32(ram,0x200,0x10000002); // branch to 0x20c
    fixture::put32(ram,0x204,0x24420001); // increment in branch delay slot
    fixture::put32(ram,0x20c,0x24630001);
    for (const auto alias : {0x1f800000U,0x9f800000U,0xbf800000U}) {
        nt::iop::Cpu cpu; cpu.pc=0x200; cpu.status=0x401; cpu.registers[8]=alias;
        nt::iop::run(cpu,ram,1,&devices);
        controller.raise(2);
        nt::iop::run(cpu,ram,1,&devices);
        check(cpu.epc==0x200 && (cpu.cause&0x80000400)==0x80000400 &&
              cpu.pc==0x80000084 && !cpu.delay_slot && !(cpu.status&1) && cpu.registers[2]==0,
              "IOP interrupt did not preserve the interrupted branch boundary");
        nt::iop::run(cpu,ram,5,&devices);
        check(cpu.pc==0x200 && cpu.status==0x401 && cpu.registers[26]==0x200 &&
              cpu.registers[27]==0x80000400 && !devices.interrupt_pending(),
              "IOP exception acknowledgement or RFE return differs");
        nt::iop::run(cpu,ram,3,&devices);
        check(cpu.registers[2]==1 && cpu.registers[3]==1,"IOP branch did not resume exactly once");
    }
    controller.raise(2);
    nt::iop::Cpu cpu; cpu.pc=0x20c; cpu.status=0x400401;
    check(nt::iop::run(cpu,ram,1,&devices).reason==nt::iop::CpuStop::interrupt_pending &&
          cpu.pc==0x20c && cpu.executed==0,"Unsupported firmware vector was executed");
    cpu.status=0x400; // IEc disabled, even though IP2 is unmasked.
    nt::iop::run(cpu,ram,1,&devices);
    check(cpu.registers[3]==1 && cpu.cause==0x400,"CPU interrupt gate or pending cause differs");
    cpu.pc=0x20c; cpu.status=0x401; cpu.load_register=4; cpu.load_value=123;
    nt::iop::run(cpu,ram,1,&devices);
    check(cpu.epc==0x20c && !(cpu.cause&0x80000000) && cpu.registers[4]==123,
          "IOP interrupt lost a preceding delayed load or retained stale BD");
}
void hilo_arithmetic() {
    nt::Bytes ram(2*1024*1024);
    struct Case { unsigned function; std::uint32_t a,b,lo,hi; };
    const Case cases[]{
        {24,0xfffffffe,3,0xfffffffa,0xffffffff}, {25,0xffffffff,0xffffffff,1,0xfffffffe},
        {26,0xfffffff9,3,0xfffffffe,0xffffffff}, {26,0x80000000,0xffffffff,0x80000000,0},
        {26,7,0,0xffffffff,7}, {26,0xfffffff9,0,1,0xfffffff9},
        {27,19,4,4,3}, {27,0xffffffff,0,0xffffffff,0xffffffff}
    };
    fixture::put32(ram,4,0x00001012); fixture::put32(ram,8,0x00001810);
    for (const auto& test : cases) {
        fixture::put32(ram,0,(4U<<21)|(5U<<16)|test.function);
        nt::iop::Cpu cpu; cpu.registers[4]=test.a; cpu.registers[5]=test.b;
        nt::iop::run(cpu,ram,1); nt::iop::run(cpu,ram,2);
        check(cpu.registers[2]==test.lo && cpu.registers[3]==test.hi && cpu.lo==test.lo && cpu.hi==test.hi,
              "IOP multiply/divide or HI/LO transfer differs");
    }
    fixture::put32(ram,0,0x00800011); fixture::put32(ram,4,0x00a00013);
    nt::iop::Cpu cpu; cpu.registers[4]=0x12345678; cpu.registers[5]=0xabcdef01;
    nt::iop::run(cpu,ram,2);
    check(cpu.hi==0x12345678 && cpu.lo==0xabcdef01,"IOP explicit HI/LO writes differ");
}
void cache_isolation() {
    nt::Bytes ram(2*1024*1024);
    nt::iop::BootstrapDevices devices;
    const std::uint32_t program[]{0x40886000,0xac890000,0xa0890001,0xa4890002,0x3c0afffe,
        0xad490130,0x8d420130,0,0x40806000,0xac890004,0x8c830004,0,0xffffffff};
    for (unsigned i=0;i<std::size(program);++i) fixture::put32(ram,i*4,program[i]);
    fixture::put32(ram,0x400,0xdeadbeef);
    nt::iop::Cpu cpu; cpu.registers[8]=0x10000; cpu.registers[4]=0x400; cpu.registers[9]=0x12345678;
    check(nt::iop::run(cpu,ram,3,&devices).reason==nt::iop::CpuStop::budget &&
          nt::le32(ram,0x400)==0xdeadbeef && cpu.status==0x10000,"Isolated writes changed RAM before suspension");
    const auto result=nt::iop::run(cpu,ram,20,&devices);
    check(result.reason==nt::iop::CpuStop::unsupported && cpu.executed==12 && cpu.status==0 &&
          nt::le32(ram,0x400)==0xdeadbeef && nt::le32(ram,0x404)==0x12345678 &&
          cpu.registers[2]==0x12345678 && cpu.registers[3]==0x12345678,
          "Cache configuration, isolated stores or restored RAM accesses differ");
    check(!devices.read(0x1ffe0130,4) && !devices.write(0xfffe0130,2,0) &&
          devices.read(0xfffe0130,4)==0x12345678,"Kernel control address was aliased or partially overwritten");
    fixture::put32(ram,0,0x8c820000);
    cpu={}; cpu.status=0x10000; cpu.registers[4]=0x400;
    const auto isolated_read=nt::iop::run(cpu,ram,1,&devices);
    check(isolated_read.reason==nt::iop::CpuStop::unsupported && isolated_read.address==0x400 &&
          cpu.executed==0,"Unsupported isolated-cache read consumed main RAM");
}
void reboot_bootstrap() {
    nt::iop::Reboot reboot(fixture::memory_bootstrap_image());
    check(reboot.advance(1).reason==nt::iop::StartupStop::budget && reboot.executed()==1,
          "Disc bootstrap did not retain budget state");
    const auto result=reboot.advance(2);
    check(result.reason==nt::iop::StartupStop::returned && result.return_value==2*1024*1024 &&
          reboot.executed()==3,"Disc bootstrap did not receive memory size or return");
    bool rejected=false;
    try { nt::iop::Reboot invalid(fixture::reboot_image()); }
    catch (const std::exception&) { rejected=true; }
    check(rejected,"Missing bootstrap module was accepted");
    nt::iop::Reboot managed(fixture::manager_bootstrap_image());
    rejected=false;
    try { managed.prepare_manager(); } catch (const std::exception&) { rejected=true; }
    check(rejected,"Manager preparation ran before memory startup");
    managed.advance(10); managed.prepare_manager();
    check(managed.manager() && managed.manager()->base==0x20030 && managed.executed()==14 &&
          nt::le32(managed.memory(),0x20030+52)==0x08004014,
          "Manager was not guest-allocated, relocated and bound to initialized memory service");
    managed.prepare_manager();
    check(managed.executed()==14,"Repeated preparation allocated another module");
    const auto manager_result=managed.run_manager(100);
    check(manager_result.reason==nt::iop::StartupStop::returned && manager_result.return_value==2 &&
          nt::le32(managed.memory(),0x10000-48)==0x20000 &&
          nt::le32(managed.memory(),0x20000+24)==0x20030,
          "Manager did not receive boot parameters or linked module records");
    const auto params=0x30000U+static_cast<unsigned>(fixture::manager_bootstrap_image().size());
    const auto pointers=nt::le32(managed.memory(),params+28);
    check(nt::le32(managed.memory(),params+12)==0x10000 && nt::le32(managed.memory(),params+16)==0x30000 &&
          nt::le32(managed.memory(),params+24)==2 && nt::le32(managed.memory(),pointers+8)==0 &&
          nt::le32(managed.memory(),nt::le32(managed.memory(),pointers))==0x464c457f,
          "Manager archive pointers or terminated module list differ");
    for (const auto address_high : {0U,1U,32U}) {
        auto image=fixture::manager_bootstrap_image();
        fixture::put32(image,96+208,0x3c020000|address_high);
        fixture::put32(image,96+212,0x03e00008); fixture::put32(image,96+216,0);
        nt::iop::Reboot invalid(image); invalid.advance(10);
        rejected=false;
        try { invalid.prepare_manager(); } catch (const std::exception&) { rejected=true; }
        check(rejected && !invalid.manager(),"Invalid or overlapping guest allocation accepted");
    }
    auto incompatible=fixture::manager_bootstrap_image(); incompatible[480+168]=3;
    nt::iop::Reboot unbound(incompatible); unbound.advance(10);
    rejected=false;
    try { unbound.prepare_manager(); } catch (const std::exception&) { rejected=true; }
    check(rejected && !unbound.manager() && nt::le32(unbound.memory(),0x20030)==0,
          "Unresolved manager imports installed executable code");
}
void module_startup() {
    // A complete original IRX: its entry reads argc/argv, uses the argument
    // save area and returns a value after executing the return delay slot.
    nt::Bytes file(160,0);
    const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(identity),std::end(identity),file.begin());
    file[16]=0x80; file[17]=0xff; file[18]=8; file[20]=1;
    fixture::put32(file,28,52); fixture::put32(file,32,84);
    file[40]=52; file[42]=32; file[44]=1; file[46]=40; file[48]=1;
    fixture::put32(file,52,1); fixture::put32(file,56,128);
    fixture::put32(file,68,32); fixture::put32(file,72,64);
    fixture::put32(file,76,7); fixture::put32(file,80,16);
    const std::uint32_t instructions[]{0xafa40000,0x8ca80000,0,0x91090000,0,0x00891021,0x03e00008,0x24420001};
    for (unsigned i=0;i<std::size(instructions);++i) fixture::put32(file,128+i*4,instructions[i]);
    const auto module=nt::elf::load_iop_module(file,0x10000);
    nt::Bytes ram(2*1024*1024,0xcc);
    nt::iop::Libraries libraries;
    const std::array<std::string,2> arguments{"alpha","beta"};
    nt::iop::Startup call(ram,module,libraries,0x1f0000,0x1ff000,arguments);
    const auto sp=call.cpu().registers[29], argv=call.cpu().registers[5];
    check(sp%16==0 && argv==sp+16 && nt::le32(ram,argv+8)==0 &&
          ram[nt::le32(ram,argv)]=='a' && ram[nt::le32(ram,argv+4)]=='b' &&
          ram[0x10020]==0 && ram[0x10040]==0xcc,"Startup layout or BSS differs");
    auto result=call.resume(7);
    check(result.reason==nt::iop::StartupStop::budget && call.cpu().delay_slot &&
          call.cpu().executed==7,"Startup did not suspend before return delay slot");
    result=call.resume(1);
    check(result.reason==nt::iop::StartupStop::returned && result.return_value==100 &&
          call.cpu().executed==8 && nt::le32(ram,sp)==2,"Loaded IRX did not return computed result");
    check(call.resume(100).return_value==100 && call.cpu().executed==8,"Completed startup executed again");
    auto direct=module;
    const std::uint32_t direct_words[]{0x00851021,0x00461021,0x00471021,0x03e00008,0};
    for (unsigned i=0;i<std::size(direct_words);++i) fixture::put32(direct.memory,i*4,direct_words[i]);
    nt::iop::Startup direct_call(ram,direct,libraries,0x1f0000,0x1ff000,
                                nt::iop::EntryRegisters{{10,20,30,40}});
    result=direct_call.resume(10);
    check(result.reason==nt::iop::StartupStop::returned && result.return_value==100 &&
          direct_call.cpu().executed==5,"Register-argument entry did not receive all four arguments");
    // A resident function increments a persistent word in its owner's BSS.
    auto resident=module;
    const std::uint32_t resident_words[]{0x8c820000,0,0x24420001,0xac820000,0x03e00008,0};
    for (unsigned i=0;i<std::size(resident_words);++i) fixture::put32(resident.memory,i*4,resident_words[i]);
    nt::iop::Startup install(ram,resident,libraries,0x1f0000,0x1ff000,
                            nt::iop::EntryRegisters{{resident.base+32,0,0,0}});
    check(install.resume(10).return_value==1,"Resident initialization did not update BSS");
    for (unsigned expected=2;expected<5;++expected) {
        nt::iop::Startup invoke(ram,resident,resident.entry,0x1f0000,0x1ff000,
                               nt::iop::EntryRegisters{{resident.base+32,0,0,0}});
        result=invoke.resume(10);
        check(result.reason==nt::iop::StartupStop::returned && result.return_value==expected &&
              nt::le32(ram,resident.base+32)==expected,"Resident call reinstalled stale module data");
    }
    const auto resident_ram=ram;
    bool invalid_entry=false;
    try { nt::iop::Startup bad(ram,resident,resident.base+32,0x1f0000,0x1ff000,nt::iop::EntryRegisters{}); }
    catch (const std::exception&) { invalid_entry=true; }
    check(invalid_entry && ram==resident_ram,"Resident call accepted BSS entry or mutated RAM on failure");
    const auto preserved=ram;
    for (const auto stack : {0x10000U,0x1ff000U}) {
        bool rejected=false;
        try { nt::iop::Startup bad(ram,module,libraries,stack,stack+16,arguments); }
        catch (const std::exception&) { rejected=true; }
        check(rejected && ram==preserved,"Invalid startup partially modified caller RAM");
    }
    const std::array<std::string,1> embedded{std::string("a\0b",3)};
    bool rejected=false;
    try { nt::iop::Startup bad(ram,module,libraries,0x1f0000,0x1ff000,embedded); }
    catch (const std::exception&) { rejected=true; }
    check(rejected && ram==preserved,"Embedded argument terminator accepted");
    auto unsupported=module; fixture::put32(unsupported.memory,0,0xffffffff);
    nt::iop::Startup failed(ram,unsupported,libraries,0x1f0000,0x1ff000,arguments);
    result=failed.resume(100);
    check(result.reason==nt::iop::StartupStop::fault && result.execution.pc==module.entry &&
          result.execution.instruction==0xffffffff && failed.cpu().executed==0,
          "Unsupported module entry falsely completed startup");
}
}
int main() {
    try { dma_interrupt_dispatch(); kernel_mode_callback(); native_interrupt_dispatch(); thread_context_switch(); resource_directory(); unaligned_words(); cdvd_initialization(); sif1_receive(); timer_interrupts(); timer_initialization(); startup_dependencies(); provider_registration(); heaplib_service(); dmacman_service(); intrman_service(); archive_dependencies(); delays(); imported_call(); status_register(); device_access(); interrupt_delivery(); hilo_arithmetic(); cache_isolation(); module_startup(); reboot_bootstrap(); std::cout<<"IOP execution, linked guest call and IRX startup passed\n"; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
