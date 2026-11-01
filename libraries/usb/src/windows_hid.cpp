#include "windows_hid.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <usbioctl.h>
#include <initguid.h>
#include <devpkey.h>
#include <algorithm>
#include <optional>
#include <cwctype>
#include <hidapi.h>

namespace nt::usb {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() { if (value!=INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
struct DeviceSet {
    HDEVINFO value;
    ~DeviceSet() { if (value!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
std::wstring driver_key(const Device& device,std::uint32_t& error) {
    const auto separator=device.key.rfind(L'#');
    if (separator==std::wstring::npos) { error=ERROR_INVALID_DATA; return {}; }
    unsigned port=0;
    for (auto i=separator+1;i<device.key.size();++i) {
        if (device.key[i]<L'0' || device.key[i]>L'9' || port>25) { error=ERROR_INVALID_DATA; return {}; }
        port=port*10+unsigned(device.key[i]-L'0');
    }
    if (!port || port>255) { error=ERROR_INVALID_DATA; return {}; }
    Handle hub{CreateFileW(device.key.substr(0,separator).c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
    if (hub.value==INVALID_HANDLE_VALUE) { error=GetLastError(); return {}; }
    std::vector<std::uint8_t> storage(65536);
    auto* key=reinterpret_cast<USB_NODE_CONNECTION_DRIVERKEY_NAME*>(storage.data());
    key->ConnectionIndex=port;
    DWORD received=0;
    if (!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,key,sizeof(*key),
                         key,static_cast<DWORD>(storage.size()),&received,nullptr)) { error=GetLastError(); return {}; }
    const auto offset=offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME,DriverKeyName);
    if (received<=offset || key->ActualLength>received || key->ActualLength<=offset) { error=ERROR_INVALID_DATA; return {}; }
    const auto count=(key->ActualLength-offset)/sizeof(wchar_t);
    const auto end=std::find(key->DriverKeyName,key->DriverKeyName+count,L'\0');
    if (end==key->DriverKeyName+count) { error=ERROR_INVALID_DATA; return {}; }
    return {key->DriverKeyName,end};
}
bool same_driver(DEVINST node,const std::wstring& key) {
    wchar_t value[512]{}; ULONG bytes=sizeof(value),type=0;
    if (CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_DRIVER,&type,value,&bytes,0)!=CR_SUCCESS || type!=REG_SZ ||
        bytes>sizeof(value) || bytes<sizeof(wchar_t) || value[bytes/sizeof(wchar_t)-1]) return false;
    return _wcsicmp(value,key.c_str())==0;
}
std::optional<unsigned> interface_number(DEVINST node) {
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(node,id,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS) return {};
    std::wstring value=id;
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c) { return std::towupper(c); });
    const auto at=value.find(L"&MI_");
    if (at==std::wstring::npos || at+6>value.size()) return {};
    unsigned result=0;
    for (auto i=at+4;i<at+6;++i) {
        const auto c=value[i];
        if (!((c>=L'0' && c<=L'9') || (c>=L'A' && c<=L'F'))) return {};
        result=result*16+unsigned(c<=L'9' ? c-L'0' : c-L'A'+10);
    }
    return result;
}
bool owns_interface(DEVINST node,const std::wstring& key,unsigned number) {
    std::optional<unsigned> found;
    for (unsigned depth=0;depth<32;++depth) {
        if (const auto part=interface_number(node)) found=part;
        if (same_driver(node,key)) return found ? *found==number : number==0;
        DEVINST parent=0;
        if (CM_Get_Parent(&parent,node,0)!=CR_SUCCESS) return false;
        node=parent;
    }
    return false;
}
class HidInput final : public InputPipe {
public:
    HidInput(HANDLE handle,unsigned bytes) : handle_{handle},bytes_(bytes),event_{CreateEventW(nullptr,TRUE,FALSE,nullptr)} {
        if (!event_.value) error_=GetLastError();
        request_.hEvent=event_.value;
    }
    ~HidInput() override { cancel(); }
    bool valid() const { return event_.value!=nullptr; }
    bool submit(std::uint32_t capacity) override {
        if (pending_) { error_=ERROR_BUSY; return false; }
        if (!capacity || capacity+1<bytes_.size()) { error_=ERROR_INSUFFICIENT_BUFFER; return false; }
        ResetEvent(event_.value); request_={}; request_.hEvent=event_.value;
        capacity_=capacity; DWORD received=0;
        if (!ReadFile(handle_.value,bytes_.data(),static_cast<DWORD>(bytes_.size()),&received,&request_)) {
            const auto error=GetLastError();
            if (error!=ERROR_IO_PENDING) { error_=error; return false; }
        }
        pending_=true; error_=0; return true;
    }
    TransferState poll(std::vector<std::uint8_t>& output) override {
        if (!pending_) { error_=ERROR_INVALID_STATE; return TransferState::failed; }
        DWORD received=0;
        if (!GetOverlappedResult(handle_.value,&request_,&received,FALSE)) {
            error_=GetLastError();
            if (error_==ERROR_IO_INCOMPLETE) return TransferState::pending;
            pending_=false; return TransferState::failed;
        }
        pending_=false;
        if (!received || received>bytes_.size()) { error_=ERROR_INVALID_DATA; return TransferState::failed; }
        // HIDClass prepends a zero report-ID byte to unnumbered reports. It is
        // not present on the USB wire; numbered report IDs are retained.
        const unsigned begin=bytes_[0]==0 ? 1U : 0U;
        if (received-begin>capacity_) { error_=ERROR_INSUFFICIENT_BUFFER; return TransferState::failed; }
        output.assign(bytes_.begin()+begin,bytes_.begin()+received); error_=0;
        return TransferState::complete;
    }
    void cancel() override {
        if (!pending_) return;
        CancelIoEx(handle_.value,&request_);
        DWORD received=0;
        GetOverlappedResult(handle_.value,&request_,&received,TRUE);
        pending_=false;
    }
    std::uint32_t error() const override { return error_; }
private:
    Handle handle_;
    std::vector<std::uint8_t> bytes_;
    Handle event_;
    OVERLAPPED request_{};
    std::uint32_t error_=0,capacity_=0;
    bool pending_=false;
};
}
namespace {
std::vector<std::wstring> hid_paths(const Device& device,unsigned number,std::uint32_t& error) {
    error=0;
    const auto key=driver_key(device,error);
    if (key.empty()) return {};
    GUID guid{}; HidD_GetHidGuid(&guid);
    DeviceSet set{SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if (set.value==INVALID_HANDLE_VALUE) { error=GetLastError(); return {}; }
    std::vector<std::wstring> paths;
    for (DWORD i=0;i<4096;++i) {
        SP_DEVICE_INTERFACE_DATA item{}; item.cbSize=sizeof(item);
        if (!SetupDiEnumDeviceInterfaces(set.value,nullptr,&guid,i,&item)) {
            if (GetLastError()!=ERROR_NO_MORE_ITEMS) { error=GetLastError(); return {}; }
            break;
        }
        DWORD needed=0;
        SetupDiGetDeviceInterfaceDetailW(set.value,&item,nullptr,0,&needed,nullptr);
        if (needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed>65536) { error=ERROR_INVALID_DATA; return {}; }
        std::vector<std::uint8_t> storage(needed);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA info{}; info.cbSize=sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set.value,&item,detail,needed,nullptr,&info)) { error=GetLastError(); return {}; }
        if (!owns_interface(info.DevInst,key,number)) continue;
        paths.emplace_back(detail->DevicePath);
    }
    if (paths.empty()) error=ERROR_NOT_SUPPORTED;
    return paths;
}
struct HidHandle {
    hid_device* value=nullptr;
    ~HidHandle() { if (value) hid_close(value); }
};
std::string utf8(const std::wstring& path) {
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),nullptr,0,nullptr,nullptr);
    if (!count) return {};
    std::string result(count,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),result.data(),count,nullptr,nullptr)) return {};
    return result;
}
std::vector<std::uint8_t> report_descriptor(const std::wstring& path) {
    const auto encoded=utf8(path);
    if (encoded.empty()) return {};
    HidHandle handle{hid_open_path(encoded.c_str())};
    if (!handle.value) return {};
    std::vector<std::uint8_t> report(HID_API_MAX_REPORT_DESCRIPTOR_SIZE);
    const auto count=hid_get_report_descriptor(handle.value,report.data(),report.size());
    if (count<=0 || static_cast<std::size_t>(count)>=report.size()) return {};
    report.resize(count); return report;
}
}
std::unique_ptr<InputPipe> open_windows_hid(const Device& device,const Endpoint& endpoint,std::uint32_t& error) {
    error=0;
    if (endpoint.interface_class!=3 || (endpoint.attributes&3)!=3 || !(endpoint.address&0x80) || endpoint.alternate) {
        error=ERROR_NOT_SUPPORTED; return {};
    }
    const auto paths=hid_paths(device,endpoint.interface_number,error);
    if (paths.empty()) return {};
    std::vector<std::unique_ptr<Handle>> handles;
    for (const auto& path:paths) {
        auto handle=std::make_unique<Handle>();
        handle->value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if (handle->value==INVALID_HANDLE_VALUE) { error=GetLastError(); return {}; }
        handles.push_back(std::move(handle));
    }
    // An endpoint needs every collection, including ones owned exclusively by
    // Windows. Report an actual open failure rather than promising a partial
    // input stream. Combining fully accessible collections is not yet supported.
    if (handles.size()!=1) { error=ERROR_NOT_SUPPORTED; return {}; }
    auto& handle=*handles.front();
    PHIDP_PREPARSED_DATA data=nullptr;
    if (!HidD_GetPreparsedData(handle.value,&data)) { error=GetLastError(); return {}; }
    HIDP_CAPS caps{};
    const auto result=HidP_GetCaps(data,&caps); HidD_FreePreparsedData(data);
    if (result!=HIDP_STATUS_SUCCESS || caps.InputReportByteLength<2 || caps.InputReportByteLength>endpoint.packet_bytes+1) {
        error=ERROR_NOT_SUPPORTED; return {};
    }
    auto input=std::make_unique<HidInput>(handle.value,caps.InputReportByteLength); handle.value=INVALID_HANDLE_VALUE;
    if (!input->valid()) { error=input->error(); return {}; }
    return input;
}
void normalize_windows_hid(Device& device) {
    auto& bytes=device.descriptors;
    if (!valid_descriptors(bytes)) return;
    unsigned number=0; bool hid=false;
    for (std::size_t at=18;at<bytes.size();at+=bytes[at]) {
        if (bytes[at+1]==4 && bytes[at]>=9) { number=bytes[at+2]; hid=bytes[at+5]==3 && bytes[at+3]==0; }
        if (bytes[at+1]==2) hid=false;
        if (!hid || bytes[at+1]!=0x21 || bytes[at]<9) continue;
        std::uint32_t error=0;
        const auto paths=hid_paths(device,number,error);
        std::vector<std::vector<std::uint8_t>> descriptors;
        for (const auto& path:paths) descriptors.push_back(report_descriptor(path));
        std::vector<std::uint8_t> report;
        if (!merge_hid_reports(descriptors,report)) continue;
        const auto count=report.size();
        for (unsigned record=6;record+3<=bytes[at];record+=3) if (bytes[at+record]==0x22) {
            bytes[at+record+1]=static_cast<std::uint8_t>(count);
            bytes[at+record+2]=static_cast<std::uint8_t>(count>>8);
            device.hid_reports[static_cast<std::uint8_t>(number)]=std::move(report); break;
        }
    }
}
bool set_windows_hid_report(const Device& device,std::uint8_t number,std::uint8_t type,std::uint8_t id,
                            std::span<const std::uint8_t> bytes,std::uint32_t& error) {
    error=0;
    if ((type!=2 && type!=3) || bytes.size()>65535) { error=ERROR_NOT_SUPPORTED; return false; }
    const auto paths=hid_paths(device,number,error);
    if (paths.empty()) return false;
    std::wstring path;
    for (const auto& candidate:paths) {
        const auto descriptor=report_descriptor(candidate);
        const auto reports=hid_report_ids(descriptor);
        if (!reports) { error=ERROR_INVALID_DATA; return false; }
        if (std::find(reports->begin(),reports->end(),HidReportId{type,id})==reports->end()) continue;
        if (!path.empty()) { error=ERROR_NOT_SUPPORTED; return false; }
        path=candidate;
    }
    if (path.empty()) { error=ERROR_NOT_SUPPORTED; return false; }
    Handle handle{CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
    if (handle.value==INVALID_HANDLE_VALUE) { error=GetLastError(); return false; }
    std::vector<std::uint8_t> report;
    if (!id) report.push_back(0);
    else if (bytes.empty() || bytes[0]!=id) { error=ERROR_INVALID_DATA; return false; }
    report.insert(report.end(),bytes.begin(),bytes.end());
    const auto result=type==2 ? HidD_SetOutputReport(handle.value,report.data(),static_cast<ULONG>(report.size())) :
                               HidD_SetFeature(handle.value,report.data(),static_cast<ULONG>(report.size()));
    if (!result) error=GetLastError();
    return result!=FALSE;
}
bool windows_usb_location(const Device& device,std::array<std::uint8_t,7>& output,std::uint32_t& error) {
    error=0; const auto key=driver_key(device,error);
    if (key.empty()) return false;
    DeviceSet set{SetupDiGetClassDevsW(nullptr,L"USB",nullptr,DIGCF_PRESENT|DIGCF_ALLCLASSES)};
    if (set.value==INVALID_HANDLE_VALUE) { error=GetLastError(); return false; }
    for (DWORD i=0;i<4096;++i) {
        SP_DEVINFO_DATA info{}; info.cbSize=sizeof(info);
        if (!SetupDiEnumDeviceInfo(set.value,i,&info)) break;
        if (!same_driver(info.DevInst,key)) continue;
        wchar_t paths[4096]{}; ULONG bytes=sizeof(paths); DEVPROPTYPE type=0;
        if (CM_Get_DevNode_PropertyW(info.DevInst,&DEVPKEY_Device_LocationPaths,&type,reinterpret_cast<PBYTE>(paths),&bytes,0)!=CR_SUCCESS ||
            type!=DEVPROP_TYPE_STRING_LIST || bytes>sizeof(paths) || paths[4095]) break;
        std::wstring path(paths); std::array<std::uint8_t,7> result{}; unsigned count=0;
        for (std::size_t at=0;(at=path.find(L"#USB(",at))!=std::wstring::npos;) {
            at+=5; unsigned port=0;
            while (at<path.size() && path[at]>=L'0' && path[at]<=L'9' && port<=25) port=port*10+unsigned(path[at++]-L'0');
            if (at==path.size() || path[at]!=L')' || !port || port>255 || count>=5) { error=ERROR_BUFFER_OVERFLOW; return false; }
            result[count++]=static_cast<std::uint8_t>(port);
        }
        if (!count) break;
        output=result; return true;
    }
    error=ERROR_NOT_SUPPORTED; return false;
}
}
#else
namespace nt::usb {
std::unique_ptr<InputPipe> open_windows_hid(const Device&,const Endpoint&,std::uint32_t& error) { error=50; return {}; }
bool windows_usb_location(const Device&,std::array<std::uint8_t,7>&,std::uint32_t& error) { error=50; return false; }
void normalize_windows_hid(Device&) {}
bool set_windows_hid_report(const Device&,std::uint8_t,std::uint8_t,std::uint8_t,std::span<const std::uint8_t>,std::uint32_t& error) { error=50; return false; }
}
#endif
