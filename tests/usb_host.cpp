#include <nt/usb.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc,char** argv) {
    try {
        std::vector<std::uint8_t> bytes{18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1,
                                       9,2,18,0,1,1,0,0x80,50,9,4,0,0,0,0xff,0,0,0};
        if (!nt::usb::valid_descriptors(bytes)) throw std::runtime_error("Valid descriptor chain rejected");
        auto malformed=bytes; malformed[27]=0;
        if (nt::usb::valid_descriptors(malformed)) throw std::runtime_error("Zero-length descriptor accepted");
        malformed=bytes; malformed[20]=17;
        if (nt::usb::valid_descriptors(malformed)) throw std::runtime_error("Truncated configuration accepted");
        malformed=bytes; malformed[17]=2;
        if (nt::usb::valid_descriptors(malformed)) throw std::runtime_error("Missing configuration accepted");
        nt::usb::Device endpoint_device;
        endpoint_device.descriptors=bytes;
        endpoint_device.descriptors[20]=25;
        endpoint_device.descriptors.insert(endpoint_device.descriptors.end(),{7,5,0x81,3,8,0,10});
        nt::usb::Endpoint endpoint;
        if (!nt::usb::describe_endpoint(endpoint_device,36,endpoint) || endpoint.address!=0x81 || endpoint.packet_bytes!=8)
            throw std::runtime_error("Endpoint interface ownership was lost");
        for (const unsigned type:{2U,4U}) {
            auto short_header=endpoint_device;
            short_header.descriptors.insert(short_header.descriptors.begin()+36,{2,static_cast<std::uint8_t>(type)});
            short_header.descriptors[20]=27;
            if (nt::usb::describe_endpoint(short_header,38,endpoint))
                throw std::runtime_error("Short descriptor supplied endpoint ownership");
        }
        const std::vector<std::uint8_t> first{5,1,9,4,0xa1,1,0x85,7,0x75,8,0x95,1,0x81,2,0xc0};
        const std::vector<std::uint8_t> second{5,1,9,5,0xa1,1,0x85,9,0x75,8,0x95,1,0x91,2,0xc0};
        std::vector<std::vector<std::uint8_t>> collections{first,second};
        std::vector<std::uint8_t> combined;
        if (!nt::usb::merge_hid_reports(collections,combined)) throw std::runtime_error("Independent HID collections did not merge");
        const auto reports=nt::usb::hid_report_ids(combined);
        if (!reports || *reports!=std::vector<nt::usb::HidReportId>{{1,7},{2,9}})
            throw std::runtime_error("HID composition lost report routing");
        const auto unchanged=combined;
        collections={first,first};
        if (nt::usb::merge_hid_reports(collections,combined) || combined!=unchanged)
            throw std::runtime_error("Conflicting HID report layouts were accepted");
        auto unnumbered=second; unnumbered.erase(unnumbered.begin()+6,unnumbered.begin()+8);
        collections={first,unnumbered};
        if (nt::usb::merge_hid_reports(collections,combined)) throw std::runtime_error("Mixed numbered and unnumbered reports accepted");
        collections={first};
        if (!nt::usb::merge_hid_reports(collections,combined) || combined!=first)
            throw std::runtime_error("Single HID collection was altered");
        auto truncated=first; truncated.pop_back();
        if (nt::usb::hid_report_ids(truncated) || nt::usb::hid_report_ids(std::vector<std::uint8_t>{0xb4}) ||
            nt::usb::hid_report_ids(std::vector<std::uint8_t>{0x85}))
            throw std::runtime_error("Malformed HID state or item was accepted");
        if (argc==2 && std::string_view(argv[1])=="--devices") {
            auto host=nt::usb::make_host(); std::vector<nt::usb::Device> devices;
            if (!host->scan(devices)) throw std::runtime_error("Windows USB query failed: "+std::to_string(host->error()));
            std::size_t bytes=0,reports=0;
            for (const auto& device:devices) {
                if (device.key.empty() || !nt::usb::valid_descriptors(device.descriptors)) throw std::runtime_error("Malformed host snapshot");
                bytes+=device.descriptors.size();
                reports+=device.hid_reports.size();
            }
            std::cout<<"Windows USB devices="<<devices.size()<<" validated_descriptor_bytes="<<bytes<<" native_hid_reports="<<reports<<'\n';
        }
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
