#include <nt/iop_reboot.hpp>
#include <nt/reboot_image.hpp>
#include <nt/iop_dependencies.hpp>
#include <nt/iop_registration.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
Reboot::Reboot(ByteView image,Sbus* sbus) : image_(image.begin(),image.end()), ram_(2*1024*1024), devices_(sbus) {
    devices_.enable_sif1_receive();
    const auto files=disc::parse_reboot_image(image_);
    const auto found=std::find_if(files.begin(),files.end(),[](const auto& file) { return file.name=="SYSMEM"; });
    if (found==files.end()) throw std::runtime_error("IOP reboot: archive has no SYSMEM bootstrap module");
    // Engine-owned bootstrap placement, independent of disc identity. Other
    // module allocations must use the memory manager after its initialization.
    memory_module_=elf::load_iop_module(ByteView(image_).subspan(found->offset,found->size),0x10000);
    call_=std::make_unique<Startup>(ram_,memory_module_,libraries_,0x1f0000,0x1ff000,
                                   EntryRegisters{{static_cast<std::uint32_t>(ram_.size()),0,0,0}},&devices_);
}
StartupResult Reboot::advance(std::uint64_t budget) { return call_->resume(budget); }
void Reboot::prepare_manager() {
    if (manager_) return;
    if (call_->resume(0).reason!=StartupStop::returned)
        throw std::runtime_error("IOP reboot: memory bootstrap has not returned");
    const auto files=disc::parse_reboot_image(image_);
    const auto found=std::find_if(files.begin(),files.end(),[](const auto& file) { return file.name=="LOADCORE"; });
    if (found==files.end()) throw std::runtime_error("IOP reboot: archive has no LOADCORE module");
    const auto file=ByteView(image_).subspan(found->offset,found->size);
    const auto sizing=elf::load_iop_module(file,0);
    // Register the initialized memory service, not every table in the archive.
    auto live=memory_module_;
    std::copy_n(ram_.begin()+live.base,live.memory.size(),live.memory.begin());
    std::optional<std::uint32_t> table;
    for (std::uint32_t offset=0;offset+20<=live.initialized_size;offset+=4) {
        if (le32(live.memory,offset)!=0x41c00000) continue;
        const auto name=ByteView(live.memory).subspan(offset+12,8);
        constexpr std::uint8_t expected[]{'s','y','s','m','e','m',0,0};
        if (!std::equal(name.begin(),name.end(),std::begin(expected))) continue;
        if (table) throw std::runtime_error("IOP reboot: ambiguous memory-service exports");
        table=offset;
    }
    if (!table) throw std::runtime_error("IOP reboot: memory-service export table is missing");
    Libraries candidate;
    candidate.register_exports(live,live.base+*table);
    const auto allocator=candidate.resolve({"sysmem",le16(live.memory,*table+8),4,0});
    if (!allocator) throw std::runtime_error("IOP reboot: memory allocation export is missing");
    allocator_=*allocator; memory_exports_=live.base+*table;
    releaser_=candidate.resolve({"sysmem",le16(live.memory,*table+8),5,0}).value_or(0);
    block_size_=candidate.resolve({"sysmem",le16(live.memory,*table+8),10,0}).value_or(0);
    // Reserve space for the public module-info record immediately before text.
    const auto allocation_size=static_cast<std::uint32_t>(sizing.memory.size()+48);
    const auto address=allocate_region(allocation_size);
    auto manager=elf::load_iop_module(file,address+48);
    candidate.bind(manager);
    std::copy(manager.memory.begin(),manager.memory.end(),ram_.begin()+manager.base);
    libraries_=std::move(candidate);
    manager_=std::move(manager);
}
std::uint32_t Reboot::memory_call(std::uint32_t entry,EntryRegisters arguments) {
    Startup call(ram_,memory_module_,entry,allocator_stack_ ? allocator_stack_ : 0x1f0000,
                 allocator_stack_ ? allocator_stack_+4096 : 0x1ff000,arguments,&devices_,&calls_);
    const auto result=call.resume(100000);
    service_executed_+=call.cpu().executed;
    if (result.reason!=StartupStop::returned)
        throw std::runtime_error("IOP reboot: memory service did not return; pc="+hex32(result.execution.pc)+
                                 " word="+hex32(result.execution.instruction));
    return result.return_value;
}
std::uint32_t Reboot::allocate_region(std::uint32_t allocation_size,unsigned policy,bool required) {
    const auto address=memory_call(allocator_,EntryRegisters{{policy,allocation_size,0,0}});
    if (!address && !required) return 0;
    if (!address || address%16 || address>=ram_.size() || allocation_size>ram_.size()-address ||
        (address<memory_module_.base+memory_module_.memory.size() && std::uint64_t(address)+allocation_size>memory_module_.base-48) ||
        std::any_of(allocations_.begin(),allocations_.end(),[&](const auto& region) {
            return address<std::uint64_t(region.first)+region.second && std::uint64_t(address)+allocation_size>region.first;
        }) ||
        (address<0x1ff000 && std::uint64_t(address)+allocation_size>0x1f0000))
        throw std::runtime_error("IOP reboot: allocator returned an invalid or overlapping module region");
    allocations_.emplace_back(address,allocation_size);
    return address;
}
HeapRegion Reboot::allocate_heap_region(std::uint32_t size,unsigned policy) {
    const auto base=allocate_region(size,policy,false);
    if (!base) return {};
    const auto actual=memory_call(block_size_,EntryRegisters{{base,0,0,0}});
    if (actual<size || actual>ram_.size()-base ||
        (base<memory_module_.base+memory_module_.memory.size() && std::uint64_t(base)+actual>memory_module_.base-48) ||
        (base<0x1ff000 && std::uint64_t(base)+actual>0x1f0000) ||
        std::any_of(allocations_.begin(),allocations_.end(),[&](const auto& region) {
            return region.first!=base && base<std::uint64_t(region.first)+region.second &&
                   region.first<std::uint64_t(base)+actual;
        })) throw std::runtime_error("IOP reboot: invalid heap backing extent");
    allocations_.back().second=actual;
    return {base,actual};
}
bool Reboot::release_heap_region(std::uint32_t base) {
    const auto found=std::find_if(allocations_.begin(),allocations_.end(),[&](const auto& region) { return region.first==base; });
    if (found==allocations_.end() || memory_call(releaser_,EntryRegisters{{base,0,0,0}})!=0) return false;
    allocations_.erase(found);
    return true;
}
void Reboot::prepare_handoff() {
    prepare_manager();
    if (memory_exports_!=memory_module_.base)
        throw std::runtime_error("IOP reboot: memory export layout cannot host bootstrap module info");
    const auto files=disc::parse_reboot_image(image_);
    std::vector<std::uint32_t> modules;
    for (const std::string name : {"SYSMEM","LOADCORE"})
        for (const auto& file : files) if (file.name==name) modules.push_back(static_cast<std::uint32_t>(file.offset));
    const auto archive_size=(image_.size()+15)&~std::size_t(15);
    Bytes registration;
    const auto dependencies=inspect_dependencies(image_);
    const auto needs=[&](std::string_view name) {
        return std::any_of(dependencies.missing_libraries.begin(),dependencies.missing_libraries.end(),
            [&](const auto& consumer) { return std::any_of(consumer.imports.begin(),consumer.imports.end(),
                [&](const auto& import) { return import.library==name; }); });
    };
    if (needs("intrman") || needs("dmacman") || needs("heaplib")) {
        std::optional<std::uint16_t> version;
        for (unsigned offset=0;offset+4<=manager_->initialized_size;offset+=4) {
            if (le32(manager_->memory,offset)!=0x41c00000) continue;
            const auto table=read_exports(*manager_,manager_->base+offset);
            if (table.name!="loadcore") continue;
            if (version || table.functions.size()<=6) throw std::runtime_error("IOP reboot: ambiguous or incomplete loadcore exports");
            version=table.version;
        }
        if (!version) throw std::runtime_error("IOP reboot: missing loadcore registration export");
        unsigned insertion=2;
        const auto add=[&](auto& provider) {
            provider.install(ram_,libraries_);
            calls_.add(provider);
            const auto adapter=registration_module(provider.module().base,provider.registration_flag(),*version);
            modules.insert(modules.begin()+insertion++,static_cast<std::uint32_t>(archive_size+registration.size()));
            registration.insert(registration.end(),adapter.begin(),adapter.end());
        };
        if (needs("intrman")) {
            intrman_=std::make_unique<Intrman>(devices_,allocate_region(Intrman::allocation_size()));
            add(*intrman_);
        }
        if (needs("dmacman")) {
            dmacman_=std::make_unique<Dmacman>(devices_,allocate_region(Dmacman::allocation_size()));
            add(*dmacman_);
        }
        if (needs("heaplib")) {
            if (!releaser_ || !block_size_) throw std::runtime_error("IOP reboot: heap backing requires complete memory exports");
            allocator_stack_=allocate_region(4096);
            heaplib_=std::make_unique<Heaplib>(allocate_region(Heaplib::allocation_size()),
                [this](auto size,auto policy) { return allocate_heap_region(size,policy); },
                [this](auto base) { return release_heap_region(base); });
            add(*heaplib_);
        }
    }
    std::vector<ModuleDependencies> pending;
    std::vector<ExportTable> available;
    for (const auto& module : dependencies.modules) {
        if (module.module=="SYSMEM" || module.module=="LOADCORE")
            available.insert(available.end(),module.exports.begin(),module.exports.end());
        else pending.push_back(module);
    }
    const auto provider=[&](const auto& service) {
        if (service) available.push_back(read_exports(service->module(),service->module().base));
    };
    provider(intrman_); provider(dmacman_); provider(heaplib_);
    const auto plan=plan_startup(pending,std::move(available));
    if (!plan.blocked.empty()) {
        std::string detail="IOP reboot: unresolved startup dependencies";
        for (const auto& module : plan.blocked) {
            detail+="; "+module.module+" unresolved_imports="+std::to_string(module.imports.size());
            if (!module.imports.empty()) {
                const auto& import=module.imports.front();
                detail+=" first="+import.library+"/"+std::to_string(import.version)+":"+std::to_string(import.ordinal);
            }
        }
        throw std::runtime_error(detail);
    }
    modules.insert(modules.end(),plan.file_offsets.begin(),plan.file_offsets.end());
    const auto image_size=archive_size+registration.size();
    const auto pointer_size=((modules.size()+1)*4+15)&~std::size_t(15);
    const auto name_of=[](const auto& module) { return module.info ? module.info->name : std::string{}; };
    const auto memory_name=name_of(memory_module_), manager_name=name_of(*manager_);
    const auto total=image_size+32+pointer_size+memory_name.size()+manager_name.size()+2;
    if (total>ram_.size()) throw std::runtime_error("IOP reboot: handoff exceeds IOP RAM");
    const auto address=allocate_region(static_cast<std::uint32_t>(total));
    const auto params=address+static_cast<std::uint32_t>(image_size), pointers=params+32;
    const auto memory_name_address=pointers+static_cast<std::uint32_t>(pointer_size);
    const auto manager_name_address=memory_name_address+static_cast<std::uint32_t>(memory_name.size()+1);
    const auto write=[&](std::uint32_t at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) ram_[at+i]=static_cast<std::uint8_t>(value>>(8*i));
    };
    std::fill_n(ram_.begin()+address,total,std::uint8_t{0});
    std::copy(image_.begin(),image_.end(),ram_.begin()+address);
    std::copy(registration.begin(),registration.end(),ram_.begin()+address+archive_size);
    for (unsigned i=0;i<modules.size();++i) write(pointers+i*4,address+modules[i]);
    std::copy(memory_name.begin(),memory_name.end(),ram_.begin()+memory_name_address);
    std::copy(manager_name.begin(),manager_name.end(),ram_.begin()+manager_name_address);
    const auto record=[&](const elf::IopModule& module,std::uint32_t name,std::uint32_t next,unsigned id) {
        const auto at=module.base-48;
        std::fill_n(ram_.begin()+at,48,std::uint8_t{0});
        write(at,next); write(at+4,name);
        write(at+8,(module.info ? module.info->version : 0)|0x30000U); write(at+12,id);
        write(at+16,module.entry); write(at+20,module.info ? module.info->global_pointer : 0);
        write(at+24,module.base); write(at+28,module.info ? module.info->text_size : module.initialized_size);
        write(at+32,module.info ? module.info->data_size : 0);
        write(at+36,module.info ? module.info->bss_size : static_cast<std::uint32_t>(module.memory.size()-module.initialized_size));
    };
    record(memory_module_,memory_name_address,manager_->base-48,1);
    record(*manager_,manager_name_address,0,2);
    // Public boot_params: RAM in MiB, mode, command, first export table,
    // archive buffer/size, module count, and a terminated ELF-pointer list.
    write(params,static_cast<std::uint32_t>(ram_.size()>>20));
    write(params+12,memory_exports_); write(params+16,address);
    write(params+20,static_cast<std::uint32_t>(image_size));
    write(params+24,static_cast<std::uint32_t>(modules.size())); write(params+28,pointers);
    manager_call_=std::make_unique<Startup>(ram_,*manager_,manager_->entry,0x1f0000,0x1ff000,EntryRegisters{{params,0,0,0}},&devices_,&calls_);
}
StartupResult Reboot::run_manager(std::uint64_t budget) {
    if (!manager_call_) prepare_handoff();
    return manager_call_->resume(budget);
}
}
