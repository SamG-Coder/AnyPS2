#pragma once
#include <nt/ee_packet_setup.hpp>
#include <nt/vu_binding.hpp>

namespace nt::ee {
// Transactional native VU1 launch owner. VU0 needs shared macro/micro state
// ownership before it can use this submission path. No host effects may occur
// inside a compiled block; graphics requests remain an explicit stop here.
class NativeVuLaunch {
public:
    explicit NativeVuLaunch(vu::Binding& binding,std::uint64_t block_budget=100000)
        : binding_(binding),block_budget_(block_budget) {
        if (binding.unit()!=1 || !block_budget) throw std::invalid_argument("Expected VU1 binding and nonzero launch budget");
    }
    vu::State state;
    vu::RunResult last_result{vu::RunStatus::unbound_program,0};
    Stop submit(unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
        if (unit!=binding_.unit()) return Stop::unsupported_vector;
        auto next=state;
        for (const auto& upload:data) {
            if (upload.destination>=1024 || upload.bytes.empty() || upload.bytes.size()%16 || upload.bytes.size()>4096)
                return Stop::invalid_state;
            for (std::size_t offset=0;offset<upload.bytes.size();offset+=16)
                std::memcpy(next.data[(upload.destination+offset/16)&1023U].data(),upload.bytes.data()+offset,16);
        }
        if (request) {
            next.input_top=static_cast<std::uint16_t>(request->top);
            next.input_itop=static_cast<std::uint16_t>(request->itop);
            next.input_valid=3;
            last_result=binding_.launch(next,request->entry,block_budget_);
            if (last_result.status!=vu::RunStatus::complete)
                return last_result.status==vu::RunStatus::unsupported_numeric ? Stop::unsupported_numeric : Stop::unsupported_vector;
        }
        state=std::move(next);
        return Stop::budget;
    }
    void connect(NativePacketSetup& packets) {
        packets.launch_context=this;
        packets.submit_compiled=[](void* raw,unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            return static_cast<NativeVuLaunch*>(raw)->submit(unit,data,request);
        };
    }
private:
    vu::Binding& binding_;
    std::uint64_t block_budget_;
};
}
