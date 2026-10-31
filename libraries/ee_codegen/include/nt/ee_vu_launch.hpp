#pragma once
#include <nt/ee_packet_setup.hpp>
#include <nt/vu_binding.hpp>
#include <nt/vu_graphics.hpp>

namespace nt::ee {
// Transactional native launch owner. VU0 shares completed vector state with EE
// macro operations. No host effects may occur
// inside a compiled block. Graphics packets are staged until the whole launch
// completes, then delivered atomically through a native consumer.
class NativeVuLaunch {
public:
    explicit NativeVuLaunch(vu::Binding& binding,std::uint64_t block_budget=100000,VectorStorage* shared=nullptr)
        : binding_(binding),block_budget_(block_budget),shared_(shared) {
        if (!block_budget || ((binding.unit()==0)!=bool(shared)))
            throw std::invalid_argument("VU0 requires shared EE storage; VU1 requires private storage");
    }
    unsigned unit() const { return binding_.unit(); }
    vu::State state;
    vu::RunResult last_result{vu::RunStatus::unbound_program,0};
    vu::GraphicsBatchConsumer graphics;
    Stop submit(unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
        if (unit!=binding_.unit()) return Stop::unsupported_vector;
        if (shared_ && shared_->active) return Stop::vector_pending;
        if (request && request->continuation && !completed_) return Stop::unsupported_vector;
        auto next=state;
        for (const auto& upload:data) {
            if (!write_vector_data(std::span(next.data).first(unit ? 1024 : 256),upload)) return Stop::invalid_state;
        }
        if (request) {
            if (shared_) {
                next.vectors=shared_->values; next.integers=shared_->integers;
                next.mac=shared_->mac; next.status=shared_->status; next.accumulator=shared_->accumulator;
            }
            next.input_top=static_cast<std::uint16_t>(request->top);
            next.input_itop=static_cast<std::uint16_t>(request->itop);
            next.input_valid=unit ? 3 : 2;
            // A new submission clears a completed graphics boundary; internal
            // graphics resumes below must retain that boundary instead.
            last_result=binding_.launch(next,request->continuation ? next.next_pair : request->entry,block_budget_);
            std::vector<vu::GraphicsPacket> packets;
            std::uint64_t blocks=last_result.blocks;
            std::size_t quadwords=0;
            while (last_result.status==vu::RunStatus::graphics_pending) {
                if (!graphics.submit) return Stop::unsupported_vector;
                vu::GraphicsPacket packet;
                try { packet=graphics::collect_packet(next.data,next.graphics_address); }
                catch (const std::invalid_argument&) { return Stop::unsupported_vector; }
                constexpr std::size_t maximum_quadwords=65536;
                if (packet.size()>maximum_quadwords-quadwords) return Stop::unsupported_vector;
                quadwords+=packet.size(); packets.push_back(std::move(packet));
                next.graphics_pending=0;
                last_result=binding_.resume(next,block_budget_-blocks);
                blocks+=last_result.blocks;
                last_result.blocks=blocks;
            }
            if (last_result.status!=vu::RunStatus::complete)
                return last_result.status==vu::RunStatus::unsupported_numeric ? Stop::unsupported_numeric : Stop::unsupported_vector;
            if (!packets.empty() && !graphics.submit(graphics.context,packets)) {
                last_result.status=vu::RunStatus::graphics_pending;
                return Stop::unsupported_vector;
            }
        }
        state=std::move(next);
        if (request) completed_=true;
        if (request && shared_) {
            shared_->values=state.vectors; shared_->integers=state.integers;
            shared_->mac=state.mac; shared_->status=state.status; shared_->accumulator=state.accumulator;
        }
        return Stop::budget;
    }
    void connect(NativePacketSetup& packets) {
        packets.launch_context=this;
        packets.submit_compiled=[](void* raw,unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            return static_cast<NativeVuLaunch*>(raw)->submit(unit,data,request);
        };
    }
    void connect_macro(Memory& memory) {
        if (!shared_) throw std::invalid_argument("Macro launch requires VU0 shared state");
        memory.vector_launch_context=this;
        memory.launch_vector=[](void* raw,bool indirect,unsigned entry) {
            auto& self=*static_cast<NativeVuLaunch*>(raw);
            const NativeLaunchView request{(indirect ? self.shared_->launch_address : entry)&511U,
                self.state.input_top,self.state.input_itop};
            return self.submit(0,{},&request);
        };
    }
    void connect_data(Memory& memory) {
        memory.vector_data[unit()]={reinterpret_cast<std::uint8_t*>(state.data.data()),
            unit() ? 16384U : 4096U,shared_ ? &shared_->active : nullptr};
    }
private:
    vu::Binding& binding_;
    std::uint64_t block_budget_;
    VectorStorage* shared_;
    bool completed_=false;
};

// Non-owning submission routing; owners must outlive the packet consumer.
class NativeVuSubmissions {
public:
    void add(NativeVuLaunch& owner) {
        auto& slot=owners_[owner.unit()];
        if (slot) throw std::invalid_argument("Duplicate VU submission owner");
        slot=&owner;
    }
    void connect(NativePacketSetup& packets) {
        packets.launch_context=this;
        packets.submit_compiled=[](void* raw,unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            if (unit>=self.owners_.size() || !self.owners_[unit]) return Stop::unsupported_vector;
            return self.owners_[unit]->submit(unit,data,request);
        };
    }
private:
    std::array<NativeVuLaunch*,2> owners_{};
};
}
