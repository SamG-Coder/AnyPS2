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
        if (argc==2 && std::string_view(argv[1])=="--devices") {
            auto host=nt::usb::make_host(); std::vector<nt::usb::Device> devices;
            if (!host->scan(devices)) throw std::runtime_error("Windows USB query failed: "+std::to_string(host->error()));
            std::size_t bytes=0;
            for (const auto& device:devices) {
                if (device.key.empty() || !nt::usb::valid_descriptors(device.descriptors)) throw std::runtime_error("Malformed host snapshot");
                bytes+=device.descriptors.size();
            }
            std::cout<<"Windows USB devices="<<devices.size()<<" validated_descriptor_bytes="<<bytes<<'\n';
        }
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
