#pragma once
#include <nt/ee_codegen.hpp>
#include <nt/ee_address.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::ee_codegen {
struct NativeAlias { std::uint32_t address,backing,bytes; };
struct NativeAccessGuard { std::uint32_t address,bytes; bool readable,writable; };
struct NativeLayout {
    std::vector<NativeAccessGuard> guards;
    std::vector<NativeAlias> aliases;
    std::vector<LayoutDeclaration> declarations;
};
inline NativeLayout prepare_native_layout(std::span<const LayoutDeclaration> input) {
    NativeLayout plan;
    struct View { std::uint32_t address,backing,bytes; bool readable,writable; };
    std::vector<View> views;
    for (const auto& record:input) {
        const auto [mask,address,first,second]=record;
        bool valid=false;
        for (auto value:{0U,0x6000U,0x1e000U,0x7e000U,0x1fe000U,0x7fe000U,0x1ffe000U}) valid|=mask==value;
        if (!valid || (address&(mask|0x1fffU)) || ((first|second)&0xfc000000U)) continue;
        const auto size=(mask+0x2000U)/2;
        const std::uint32_t backing[]{(first&0x03ffffc0U)<<6,(second&0x03ffffc0U)<<6};
        if (std::uint64_t(address)+2*size>0x100000000ULL || backing[0]%size || backing[1]%size) continue;
        const std::uint32_t entries[]{first,second};
        bool supported=true;
        for (unsigned half=0;half<2;++half) {
            const bool ram=std::uint64_t(backing[half])+size<=0x2000000U && (entries[half]&6)==6;
            const bool port=address+half*size==backing[half] && backing[half]>=0x10000000U &&
                std::uint64_t(backing[half])+size<=0x20000000U;
            supported&=ram || port;
        }
        if (!supported) continue;
        for (unsigned half=0;half<2;++half) {
            const auto start=address+half*size;
            const bool readable=(entries[half]&2)!=0,writable=(entries[half]&6)==6;
            for (const auto& previous:views) {
                const auto begin=std::max(start,previous.address);
                const auto end=std::min(std::uint64_t(start)+size,std::uint64_t(previous.address)+previous.bytes);
                if (begin<end && (std::uint64_t(begin-start)+backing[half]!=
                    std::uint64_t(begin-previous.address)+previous.backing ||
                    readable!=previous.readable || writable!=previous.writable))
                    throw std::runtime_error("Conflicting native layout candidates");
            }
            views.push_back({start,backing[half],size,readable,writable});
            if (backing[half]>=0x10000000U) {
                if (!readable || !writable) plan.guards.push_back({start,size,readable,writable});
                continue;
            }
            for (std::uint32_t offset=0;offset<size;offset+=4096) {
                const auto raw=start+offset,old=runtime::default_ram_address(raw);
                if (old<0x2000000U && old!=backing[half]+offset)
                    throw std::runtime_error("Native layout would redirect existing RAM");
                if (old>=0x2000000U && raw<0x20000000U)
                    throw std::runtime_error("Native layout would redirect native device storage");
            }
            if (runtime::default_ram_address(start)==backing[half] &&
                runtime::default_ram_address(start+size-1)==backing[half]+size-1) continue;
            const NativeAlias next{start,backing[half],size};
            for (const auto& previous:plan.aliases) {
                const auto begin=std::max(next.address,previous.address);
                const auto end=std::min(std::uint64_t(next.address)+next.bytes,std::uint64_t(previous.address)+previous.bytes);
                if (begin<end && std::uint64_t(begin-next.address)+next.backing!=
                    std::uint64_t(begin-previous.address)+previous.backing)
                    throw std::runtime_error("Conflicting native alias candidates");
            }
            plan.aliases.push_back(next);
        }
        if (std::find(plan.declarations.begin(),plan.declarations.end(),record)==plan.declarations.end())
            plan.declarations.push_back(record);
    }
    std::sort(plan.aliases.begin(),plan.aliases.end(),[](auto a,auto b){ return a.address<b.address; });
    std::vector<NativeAlias> merged;
    for (const auto& next:plan.aliases) {
        if (!merged.empty()) {
            auto& previous=merged.back();
            if (std::uint64_t(next.address)<=std::uint64_t(previous.address)+previous.bytes &&
                std::uint64_t(next.address-previous.address)+previous.backing==next.backing) {
                previous.bytes=static_cast<std::uint32_t>(std::max(std::uint64_t(previous.address)+previous.bytes,
                    std::uint64_t(next.address)+next.bytes)-previous.address);
                continue;
            }
        }
        merged.push_back(next);
    }
    plan.aliases=std::move(merged);
    return plan;
}
}
