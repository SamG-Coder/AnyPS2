#pragma once
#include <nt/usb.hpp>
namespace nt::usb {
std::unique_ptr<InputPipe> open_windows_hid(const Device&,const Endpoint&,std::uint32_t& error);
bool windows_usb_location(const Device&,std::array<std::uint8_t,7>&,std::uint32_t& error);
void normalize_windows_hid(Device&);
bool set_windows_hid_report(const Device&,std::uint8_t interface_number,std::uint8_t type,std::uint8_t id,
                            std::span<const std::uint8_t>,std::uint32_t& error);
}
