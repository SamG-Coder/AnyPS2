#include <nt/usb.hpp>
#include <algorithm>

namespace nt::usb {
std::optional<std::vector<HidReportId>> hid_report_ids(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size()>65535) return {};
    std::vector<HidReportId> reports;
    std::vector<std::uint8_t> globals;
    std::uint8_t id=0;
    unsigned collections=0;
    for (std::size_t at=0;at<bytes.size();) {
        const auto prefix=bytes[at++];
        const unsigned size=(prefix&3)==3 ? 4 : prefix&3,type=(prefix>>2)&3,tag=prefix>>4;
        if (prefix==0xfe || type==3 || size>bytes.size()-at) return {};
        if (type==1 && tag==8) {
            if (size!=1 || !bytes[at]) return {};
            id=bytes[at];
        } else if (type==1 && tag==10) {
            if (size || globals.size()>=32) return {};
            globals.push_back(id);
        } else if (type==1 && tag==11) {
            if (size || globals.empty()) return {};
            id=globals.back(); globals.pop_back();
        } else if (type==0) {
            if (tag==10) {
                if (size!=1 || ++collections>64) return {};
            } else if (tag==12) {
                if (size || !collections) return {};
                --collections;
            } else if (tag==8 || tag==9 || tag==11) {
                if (!size || !collections) return {};
                const HidReportId report{static_cast<std::uint8_t>(tag==8 ? 1 : tag==9 ? 2 : 3),id};
                if (std::find(reports.begin(),reports.end(),report)==reports.end()) reports.push_back(report);
            } else return {};
        }
        at+=size;
    }
    if (collections || !globals.empty() || reports.empty()) return {};
    return reports;
}
bool merge_hid_reports(std::span<const std::vector<std::uint8_t>> descriptors,std::vector<std::uint8_t>& output) {
    if (descriptors.empty() || descriptors.size()>64) return false;
    std::vector<HidReportId> all;
    std::size_t size=descriptors.size()>1 ? descriptors.size()*2 : 0;
    for (const auto& bytes:descriptors) {
        const auto reports=hid_report_ids(bytes);
        if (!reports || bytes.size()>65535-size) return false;
        size+=bytes.size();
        for (const auto& report:*reports) {
            if (std::find(all.begin(),all.end(),report)!=all.end()) return false;
            all.push_back(report);
        }
    }
    const bool numbered=std::any_of(all.begin(),all.end(),[](const auto& report) { return report.id!=0; });
    if (numbered && std::any_of(all.begin(),all.end(),[](const auto& report) { return report.id==0; })) return false;
    std::vector<std::uint8_t> combined;
    combined.reserve(size);
    for (const auto& bytes:descriptors) {
        if (descriptors.size()>1) combined.push_back(0xa4); // HID global Push.
        combined.insert(combined.end(),bytes.begin(),bytes.end());
        if (descriptors.size()>1) combined.push_back(0xb4); // Restore collection defaults.
    }
    output=std::move(combined); return true;
}
}
