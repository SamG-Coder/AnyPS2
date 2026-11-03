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
    unsigned last_entry=0,last_next_pair=0;
    // Failure-only diagnostic snapshot; never used to resume execution.
    std::unique_ptr<vu::State> failed_state;
    vu::GraphicsBatchConsumer graphics;
    Stop submit_sequence(unsigned unit,std::span<const NativeSubmissionStep> steps) {
        if (unit!=binding_.unit() || steps.empty() || steps.size()>NativePacketBatch::maximum_descriptors) return Stop::unsupported_vector;
        if (shared_ && shared_->active) return Stop::vector_pending;
        // Reuse the ordinary compiled launch path against private candidate
        // state. No graphics or shared EE state is published until every step
        // succeeds, including continuation and code-identity validation.
        auto candidate_binding=binding_;
        VectorStorage candidate_shared{};
        if (shared_) candidate_shared=*shared_;
        NativeVuLaunch candidate(candidate_binding,block_budget_,shared_ ? &candidate_shared : nullptr);
        candidate.state=state; candidate.completed_=completed_;
        struct Output { std::vector<graphics::Packet> packets; std::size_t quadwords=0; } output;
        candidate.graphics={&output,[](void* raw,std::span<const graphics::Packet> packets) {
            auto& out=*static_cast<Output*>(raw);
            for (const auto& packet:packets) {
                if (packet.size()>NativePacketBatch::maximum_bytes/16-out.quadwords) return false;
                out.quadwords+=packet.size(); out.packets.push_back(packet);
            }
            return true;
        }};
        for (const auto& step:steps) {
            if (step.launch && !candidate.block_budget_) return Stop::unsupported_vector;
            const auto request=step.launch ? &*step.launch : nullptr;
            const auto result=step.uploads.empty() ? candidate.submit(unit,step.data,request,step.drawing) :
                candidate.submit_uploaded(unit,step.uploads,step.data,request,step.drawing);
            last_result=candidate.last_result; last_entry=candidate.last_entry; last_next_pair=candidate.last_next_pair;
            if (result!=Stop::budget) { failed_state=std::move(candidate.failed_state); return result; }
            if (step.launch) candidate.block_budget_-=candidate.last_result.blocks;
        }
        if (!output.packets.empty() && (!graphics.submit || !graphics.submit(graphics.context,output.packets)))
            return Stop::unsupported_vector;
        binding_=std::move(candidate_binding); state=std::move(candidate.state); completed_=candidate.completed_;
        if (shared_) *shared_=candidate_shared;
        return Stop::budget;
    }
    Stop submit_uploaded(unsigned unit,std::span<const NativeUploadView> uploads,
                         std::span<const NativeVectorDataView> data,const NativeLaunchView* request,
                         std::span<const graphics::Packet> prefix={}) {
        std::vector<vu::UploadView> views;
        views.reserve(uploads.size());
        for (const auto& upload:uploads) views.push_back({upload.destination,upload.bytes});
        auto previous=binding_;
        if (!binding_.upload(unit,views)) return Stop::unsupported_vector;
        const auto result=submit(unit,data,request,prefix);
        if (result!=Stop::budget) binding_=std::move(previous);
        return result;
    }
    Stop submit(unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request,
                std::span<const graphics::Packet> prefix={}) {
        if (unit!=binding_.unit()) return Stop::unsupported_vector;
        if (shared_ && shared_->active) return Stop::vector_pending;
        if (request && request->continuation && !completed_) return Stop::unsupported_vector;
        auto next=state;
        for (const auto& upload:data) {
            if (!write_vector_data(std::span(next.data).first(unit ? 1024 : 256),upload)) return Stop::invalid_state;
        }
        std::vector<vu::GraphicsPacket> packets(prefix.begin(),prefix.end());
        constexpr std::size_t maximum_quadwords=65536;
        std::size_t quadwords=0;
        for (const auto& packet:packets) {
            if (packet.size()>maximum_quadwords-quadwords) return Stop::unsupported_vector;
            quadwords+=packet.size();
        }
        if (request) {
            if (shared_) {
                next.vectors=shared_->values; next.integers=shared_->integers;
                next.mac=shared_->mac; next.status=shared_->status; next.accumulator=shared_->accumulator;
                next.quotient=shared_->quotient; next.clip=shared_->clip;
                next.random=shared_->random; next.immediate=shared_->immediate;
            }
            next.input_top=static_cast<std::uint16_t>(request->top);
            next.input_itop=static_cast<std::uint16_t>(request->itop);
            next.input_valid=unit ? 3 : 2;
            // A new submission clears a completed graphics boundary; internal
            // graphics resumes below must retain that boundary instead.
            last_entry=request->continuation ? next.next_pair : request->entry;
            last_result=binding_.launch(next,last_entry,block_budget_);
            last_next_pair=next.next_pair;
            std::uint64_t blocks=last_result.blocks;
            while (last_result.status==vu::RunStatus::graphics_pending) {
                if (!graphics.submit) return Stop::unsupported_vector;
                vu::GraphicsPacket packet;
                try { packet=graphics::collect_packet(next.data,next.graphics_address); }
                catch (const std::invalid_argument&) { return Stop::unsupported_vector; }
                if (packet.size()>maximum_quadwords-quadwords) return Stop::unsupported_vector;
                quadwords+=packet.size(); packets.push_back(std::move(packet));
                next.graphics_pending=0;
                last_result=binding_.resume(next,block_budget_-blocks);
                last_next_pair=next.next_pair;
                blocks+=last_result.blocks;
                last_result.blocks=blocks;
            }
            if (last_result.status!=vu::RunStatus::complete) {
                failed_state=std::make_unique<vu::State>(next);
                return last_result.status==vu::RunStatus::unsupported_numeric ? Stop::unsupported_numeric : Stop::unsupported_vector;
            }
        }
        if (!packets.empty()) {
            if (!graphics.submit || !graphics.submit(graphics.context,packets)) {
                last_result.status=vu::RunStatus::graphics_pending;
                return Stop::unsupported_vector;
            }
        }
        state=std::move(next);
        if (request) completed_=true;
        if (request && shared_) {
            shared_->values=state.vectors; shared_->integers=state.integers;
            shared_->mac=state.mac; shared_->status=state.status; shared_->accumulator=state.accumulator;
            shared_->quotient=state.quotient; shared_->clip=state.clip;
            shared_->random=state.random; shared_->immediate=state.immediate;
        }
        return Stop::budget;
    }
    void connect(NativePacketSetup& packets) {
        packets.launch_context=this;
        packets.submit_sequence=[](void* raw,unsigned unit,std::span<const NativeSubmissionStep> steps) {
            return static_cast<NativeVuLaunch*>(raw)->submit_sequence(unit,steps);
        };
        packets.submit_compiled=[](void* raw,unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            return static_cast<NativeVuLaunch*>(raw)->submit(unit,data,request);
        };
        packets.submit_uploaded=[](void* raw,unsigned unit,std::span<const NativeUploadView> uploads,
                                   std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            return static_cast<NativeVuLaunch*>(raw)->submit_uploaded(unit,uploads,data,request);
        };
        packets.submit_combined=[](void* raw,unsigned unit,std::span<const NativeUploadView> uploads,
                                  std::span<const NativeVectorDataView> data,const NativeLaunchView* request,std::span<const graphics::Packet> drawing) {
            auto& owner=*static_cast<NativeVuLaunch*>(raw);
            return uploads.empty() ? owner.submit(unit,data,request,drawing) : owner.submit_uploaded(unit,uploads,data,request,drawing);
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
        packets.submit_sequence=[](void* raw,unsigned unit,std::span<const NativeSubmissionStep> steps) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            if (unit>=self.owners_.size() || !self.owners_[unit]) return Stop::unsupported_vector;
            return self.owners_[unit]->submit_sequence(unit,steps);
        };
        packets.submit_compiled=[](void* raw,unsigned unit,std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            if (unit>=self.owners_.size() || !self.owners_[unit]) return Stop::unsupported_vector;
            return self.owners_[unit]->submit(unit,data,request);
        };
        packets.submit_uploaded=[](void* raw,unsigned unit,std::span<const NativeUploadView> uploads,
                                   std::span<const NativeVectorDataView> data,const NativeLaunchView* request) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            if (unit>=self.owners_.size() || !self.owners_[unit]) return Stop::unsupported_vector;
            return self.owners_[unit]->submit_uploaded(unit,uploads,data,request);
        };
        packets.submit_combined=[](void* raw,unsigned unit,std::span<const NativeUploadView> uploads,
                                  std::span<const NativeVectorDataView> data,const NativeLaunchView* request,std::span<const graphics::Packet> drawing) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            if (unit>=self.owners_.size() || !self.owners_[unit]) return Stop::unsupported_vector;
            auto& owner=*self.owners_[unit];
            return uploads.empty() ? owner.submit(unit,data,request,drawing) : owner.submit_uploaded(unit,uploads,data,request,drawing);
        };
    }
private:
    std::array<NativeVuLaunch*,2> owners_{};
};
}
