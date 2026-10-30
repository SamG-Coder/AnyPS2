#pragma once
#include <nt/ee_packet_setup.hpp>
#include <nt/vu_binding.hpp>

namespace nt::ee {
inline Stop accept_compiled_uploads(vu::Binding& binding,unsigned unit,std::span<const NativeUploadView> uploads) {
    std::vector<vu::UploadView> views;
    views.reserve(uploads.size());
    for (const auto& upload:uploads) views.push_back({upload.destination,upload.bytes});
    return binding.upload(unit,views) ? Stop::budget : Stop::unsupported_vector;
}
// The caller owns the binding and must keep it alive while packets can arrive.
inline void bind_uploads(NativePacketSetup& packets,vu::Binding& binding) {
    packets.compiled_context=&binding;
    packets.accept_uploads=[](void* context,unsigned unit,std::span<const NativeUploadView> uploads) {
        return accept_compiled_uploads(*static_cast<vu::Binding*>(context),unit,uploads);
    };
}
// Non-owning unit routing; bindings must outlive this router and its packet owner.
class NativeVuUploads {
public:
    void add(vu::Binding& binding) {
        auto& slot=bindings_[binding.unit()];
        if (slot) throw std::invalid_argument("Duplicate compiled VU unit binding");
        slot=&binding;
    }
    void connect(NativePacketSetup& packets) {
        packets.compiled_context=this;
        packets.accept_uploads=[](void* context,unsigned unit,std::span<const NativeUploadView> uploads) {
            auto& self=*static_cast<NativeVuUploads*>(context);
            if (unit>=self.bindings_.size() || !self.bindings_[unit]) return Stop::unsupported_vector;
            return accept_compiled_uploads(*self.bindings_[unit],unit,uploads);
        };
    }
private:
    std::array<vu::Binding*,2> bindings_{};
};
}
