#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace nt::iop {
struct NativeLibrarySpec {
    std::uint32_t address;
    std::uint16_t version;
    std::string name;
    std::vector<std::uint32_t> functions;
    std::uint32_t module_entry=0;
};
struct NativeImportSpec {
    std::uint32_t address, table;
    std::string name;
    std::uint16_t version, ordinal;
};
// The converter supplies these descriptors only after compiling every export.
// Registration is a host operation; guest linked lists and patchable stubs are
// not installed. Discovery alone never makes an export visible to clients.
class NativeLibraryServices final : public NativeServices {
public:
    explicit NativeLibraryServices(std::vector<NativeLibrarySpec> libraries,std::vector<NativeImportSpec> imports={})
        : libraries_(std::move(libraries)), active_(libraries_.size()), modes_(libraries_.size()), imports_(std::move(imports)),
          bindings_(imports_.size()), enabled_(imports_.size(),true) {
        for (std::size_t index=0;index<imports_.size();++index) {
            const auto& import=imports_[index];
            if (import.address%4 || import.address>0x1ffff8 || import.table%4 || import.table>import.address ||
                import.address-import.table<20 || import.name.empty() || import.name.size()>8)
                throw std::invalid_argument("Invalid compiled import descriptor");
            for (std::size_t other=0;other<index;++other)
                if (imports_[other].address==import.address) throw std::invalid_argument("Duplicate compiled import stub");
        }
        for (std::size_t index=0;index<libraries_.size();++index) {
            const auto& library=libraries_[index];
            if (library.module_entry%4 || library.module_entry>=0x200000 || library.address%4 || library.address>=0x200000 || library.name.empty() || library.name.size()>8 ||
                library.functions.size()>65536 || 24+library.functions.size()*4>0x200000-library.address)
                throw std::invalid_argument("Invalid compiled export descriptor");
            for (const auto target : library.functions)
                if (!target || target%4 || target>=0x200000) throw std::invalid_argument("Invalid compiled export target");
            for (std::size_t other=0;other<index;++other)
                if (libraries_[other].address==library.address) throw std::invalid_argument("Duplicate compiled export table");
        }
    }
    std::optional<std::uint32_t> resolve(std::string_view name,std::uint16_t version,unsigned ordinal) const {
        const auto selected=select_library(name,version);
        if (!selected || ordinal>=selected->functions.size()) return {};
        return selected->functions[ordinal];
    }
    std::optional<std::uint32_t> declaration_for_entry(std::string_view name,unsigned major,std::uint32_t entry) const {
        const auto address=ram_offset(entry,4,0x200000);
        if (!address) return {};
        for (const auto& library:libraries_)
            if (library.name==name && (library.version>>8)==major && !library.functions.empty() && (library.module_entry ? library.module_entry : library.functions.front())==*address)
                return library.address;
        return {};
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind==NativeServiceKind::library_call) {
            const auto site=ram_offset(request.call_site,4,0x200000);
            if (!site) return {};
            for (std::size_t index=0;index<imports_.size();++index)
                if (imports_[index].address==*site && imports_[index].ordinal==request.selector && bindings_[index])
                    return {NativeServiceStatus::transferred,libraries_[*bindings_[index]].functions[request.selector]};
            return {};
        }
        if (request.kind!=NativeServiceKind::loadcore || request.selector<6 || (request.selector>11 && request.selector!=27)) return {};
        if ((request.selector==10 || request.selector==27) && !request.arguments[0]) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-214)};
        const auto address=ram_offset(request.arguments[0],4,ram.size());
        if (!address) return {NativeServiceStatus::address_fault,0,request.arguments[0]};
        if (request.selector==11) {
            if (20>ram.size()-*address || 20>0x200000-*address)
                return {NativeServiceStatus::address_fault,0,request.arguments[0]};
            const auto version=load_native_word(ram,*address+8)&65535;
            std::string name;
            for (unsigned byte=0;byte<8 && ram[*address+12+byte];++byte) name.push_back(static_cast<char>(ram[*address+12+byte]));
            const auto selected=select_library(name,static_cast<std::uint16_t>(version));
            return {NativeServiceStatus::returned,selected ? selected->address+20 : 0};
        }
        if (request.selector==8 || request.selector==9) {
            const auto size=request.arguments[1];
            if (size>ram.size()-*address || size>0x200000-*address)
                return {NativeServiceStatus::address_fault,0,request.arguments[0]};
            const auto scanned=size&~3U;
            auto pending=bindings_;
            for (std::size_t index=0;index<imports_.size();++index)
                if (imports_[index].table>=*address && imports_[index].table-*address<scanned) {
                    pending[index]=request.selector==8 ? select(imports_[index]) : std::optional<std::size_t>{};
                    if (request.selector==8 && !pending[index]) return {NativeServiceStatus::returned,0xffffffff};
                }
            bindings_.swap(pending);
            for (std::size_t index=0;index<imports_.size();++index)
                if (imports_[index].table>=*address && imports_[index].table-*address<scanned) enabled_[index]=request.selector==8;
            return {NativeServiceStatus::returned,0};
        }
        const auto found=std::find_if(libraries_.begin(),libraries_.end(),[&](const auto& library) { return library.address==*address; });
        const auto reply=[](std::int32_t value) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (found==libraries_.end()) return reply((request.selector==6 || request.selector==10) ? -214 : -213);
        const auto index=static_cast<std::size_t>(found-libraries_.begin());
        if (request.selector==7) {
            if (!active_[index]) return reply(-213);
            if (std::find(bindings_.begin(),bindings_.end(),std::optional<std::size_t>{index})!=bindings_.end()) return reply(-215);
            active_[index]=false; return reply(0);
        }
        const auto bytes=24+found->functions.size()*4;
        if (bytes>ram.size()-*address) return {NativeServiceStatus::address_fault,0,request.arguments[0]};
        // Validate the live declaration against the compiled descriptor before
        // exposing any function. Mutable data cannot introduce executable code.
        if (load_native_word(ram,*address)!=0x41c00000 || load_native_word(ram,*address+4)!=0 ||
            load_native_word(ram,*address+8)!=(found->version|(std::uint32_t(modes_[index])<<16))) return reply(-214);
        for (unsigned byte=0;byte<found->name.size();++byte)
            if (ram[*address+12+byte]!=static_cast<unsigned char>(found->name[byte])) return reply(-214);
        if (found->name.size()<8 && ram[*address+12+found->name.size()]!=0) return reply(-214);
        for (std::size_t ordinal=0;ordinal<found->functions.size();++ordinal)
            if (load_native_word(ram,*address+20+static_cast<std::uint32_t>(ordinal)*4)!=found->functions[ordinal]) return reply(-214);
        if (load_native_word(ram,*address+20+static_cast<std::uint32_t>(found->functions.size())*4)) return reply(-214);
        if (request.selector==27) {
            if (nt_iop_native_code_word(*address+8)) return {NativeServiceStatus::code_write,0,request.arguments[0]+8};
            const auto mode=static_cast<std::uint16_t>((modes_[index]&~6U)|(request.arguments[1]&6U));
            store_native_word(ram,*address+8,found->version|(std::uint32_t(mode)<<16));
            modes_[index]=mode;
            return reply(0);
        }
        if (request.selector==10) {
            if (active_[index]) return reply(-214);
            if (nt_iop_native_code_word(*address+8)) return {NativeServiceStatus::code_write,0,request.arguments[0]+8};
            modes_[index]|=1;
            store_native_word(ram,*address+8,found->version|(std::uint32_t(modes_[index])<<16));
            active_[index]=true;
            return reply(0);
        }
        for (std::size_t other=0;other<libraries_.size();++other)
            if (active_[other] && libraries_[other].name==found->name && (libraries_[other].version>>8)==(found->version>>8) &&
                libraries_[other].version>=found->version) return reply(-212);
        if (modes_[index]&1) {
            if (nt_iop_native_code_word(*address+8)) return {NativeServiceStatus::code_write,0,request.arguments[0]+8};
            modes_[index]&=static_cast<std::uint16_t>(~1U);
            store_native_word(ram,*address+8,found->version|(std::uint32_t(modes_[index])<<16));
        }
        active_[index]=true;
        for (std::size_t client=0;client<imports_.size();++client)
            if (enabled_[client]) bindings_[client]=select(imports_[client]);
        return reply(0);
    }
private:
    std::vector<NativeLibrarySpec> libraries_;
    std::vector<bool> active_;
    std::vector<std::uint16_t> modes_;
    std::vector<NativeImportSpec> imports_;
    std::vector<std::optional<std::size_t>> bindings_;
    std::vector<bool> enabled_;
    const NativeLibrarySpec* select_library(std::string_view name,std::uint16_t version) const {
        const NativeLibrarySpec* selected=nullptr;
        for (std::size_t index=0;index<libraries_.size();++index) {
            const auto& library=libraries_[index];
            if (active_[index] && library.name==name && (library.version>>8)==(version>>8) &&
                (!selected || library.version>selected->version)) selected=&library;
        }
        return selected;
    }
    std::optional<std::size_t> select(const NativeImportSpec& import) const {
        std::optional<std::size_t> selected;
        for (std::size_t index=0;index<libraries_.size();++index) {
            const auto& library=libraries_[index];
            if (active_[index] && !(modes_[index]&1) && library.name==import.name && (library.version>>8)==(import.version>>8) &&
                import.ordinal<library.functions.size() && (!selected || library.version>libraries_[*selected].version)) selected=index;
        }
        return selected;
    }
};
// Explicit service composition keeps ownership and dispatch out of generated code.
class NativeServiceRouter final : public NativeServices {
public:
    explicit NativeServiceRouter(std::vector<NativeServices*> services) : services_(std::move(services)) {
        for (const auto service : services_) if (!service) throw std::invalid_argument("Null native service provider");
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        for (const auto service : services_) {
            const auto reply=service->invoke(request,ram);
            if (reply.status!=NativeServiceStatus::unsupported) return reply;
        }
        return {};
    }
private:
    std::vector<NativeServices*> services_;
};
}
