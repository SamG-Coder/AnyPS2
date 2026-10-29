#pragma once
#include <nt/iop_native_services.hpp>
#include <vector>
#include <stdexcept>

namespace nt::iop {
struct NativeBootMode {
    std::uint16_t value;
    std::uint8_t id;
    std::vector<std::uint32_t> data;
};
// Host-supplied boot context only. No console mode, firmware version or reboot
// argument is fabricated. The owner reserves storage before constructing this
// service; callers receive stable data-arena pointers, never host pointers.
class NativeBootServices final : public NativeServices {
public:
    NativeBootServices()=default; // Explicitly empty context.
    NativeBootServices(std::span<const NativeBootMode> modes,std::span<std::uint8_t> ram,
                       std::uint32_t storage,std::uint32_t capacity) {
        const auto base=ram_offset(storage,4,ram.size());
        if (!base || !*base || capacity>ram.size()-*base || capacity>0x200000-*base)
            throw std::invalid_argument("Invalid native boot context storage");
        std::uint64_t required=4;
        for (const auto& mode : modes) {
            if (mode.data.size()>255 || (!mode.value && !mode.id && mode.data.empty()))
                throw std::invalid_argument("Invalid native boot record");
            required+=4+mode.data.size()*4;
        }
        if (required>capacity) throw std::invalid_argument("Native boot records exceed reserved storage");
        // Allocate host bookkeeping before mutating the supplied arena.
        records_.reserve(modes.size());
        auto cursor=*base;
        for (const auto& mode : modes) {
            records_.push_back({cursor,static_cast<std::uint32_t>(mode.data.size())});
            store_native_word(ram,cursor,mode.value|(std::uint32_t(mode.id)<<16)|(std::uint32_t(mode.data.size())<<24));
            cursor+=4;
            for (const auto word : mode.data) { store_native_word(ram,cursor,word); cursor+=4; }
        }
        store_native_word(ram,cursor,0);
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::loadcore || request.selector!=12) return {};
        for (const auto& record : records_) {
            if (record.address>ram.size() || 4+record.words*4>ram.size()-record.address)
                return {NativeServiceStatus::address_fault,0,record.address};
            const auto header=load_native_word(ram,record.address);
            if (!header) break;
            if ((header>>24)!=record.words) return {NativeServiceStatus::address_fault,0,record.address};
            if (((header>>16)&255)==request.arguments[0]) return {NativeServiceStatus::returned,record.address};
        }
        return {NativeServiceStatus::returned,0};
    }
private:
    struct Record { std::uint32_t address, words; };
    std::vector<Record> records_;
};
}
