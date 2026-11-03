#pragma once
#include <nt/ee_packet_setup.hpp>
#include <nt/vu_binding.hpp>
#include <nt/vu_graphics.hpp>
#include <chrono>
#include <future>
#include <mutex>

namespace nt::ee {
// Native launch owner. VU0 shares completed vector state with EE macro
// operations. Compiled blocks have no host effects. Asynchronous VU1 jobs
// publish owned graphics batches after each completed launch, allowing native
// drawing to overlap subsequent compiled work. Shared vector state and DMA
// completion remain private until the complete submission succeeds.
class NativeVuLaunch {
public:
    explicit NativeVuLaunch(vu::Binding& binding,std::uint64_t block_budget=100000,VectorStorage* shared=nullptr,
                            std::uint64_t sequence_budget=1000000)
        : binding_(binding),block_budget_(block_budget),sequence_budget_(sequence_budget),shared_(shared) {
        if (!block_budget || !sequence_budget || ((binding.unit()==0)!=bool(shared)))
            throw std::invalid_argument("VU0 requires shared EE storage; VU1 requires private storage");
    }
    struct Profile {
        std::uint64_t compiled_ns=0,blocks=0,jobs=0,job_ns=0,submit_ns=0;
        std::uint64_t longest_job_ns=0,longest_submit_ns=0;
        vu::ExecutionProfile execution;
    };
    Profile* profile=nullptr;
    unsigned unit() const { return binding_.unit(); }
    vu::State state;
    vu::RunResult last_result{vu::RunStatus::unbound_program,0};
    unsigned last_entry=0,last_next_pair=0;
    struct SequenceWork { std::uint64_t launches=0,blocks=0,largest_launch=0; } last_sequence;
    // Failure-only diagnostic snapshot; never used to resume execution.
    std::unique_ptr<vu::State> failed_state;
    vu::GraphicsBatchConsumer graphics;
    void enable_async() {
        if (unit()!=1 || shared_) throw std::invalid_argument("Asynchronous launches require private VU1 state");
        asynchronous_=true;
    }
    bool pending() const { return busy_; }
    Stop poll() {
        if (!busy_) return job_status_;
        // Drain completed launches while later compiled launches are running.
        // The owner alone calls the renderer; the producer only moves owned data.
        const bool ready=job_.wait_for(std::chrono::seconds(0))==std::future_status::ready;
        auto output=stream_->take();
        const auto began=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto accepted=accept_packets(output);
        if (profile) {
            const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
            profile->submit_ns+=elapsed; profile->longest_submit_ns=std::max(profile->longest_submit_ns,elapsed);
        }
        if (!accepted) { job_status_=Stop::unsupported_vector; job_.wait(); }
        if (!ready && accepted) return Stop::budget;
        auto result=job_.get();
        busy_=false;
        last_result=result.last_result; last_entry=result.entry; last_next_pair=result.next;
        last_sequence=result.sequence; failed_state=std::move(result.failed);
        if (profile) {
            profile->compiled_ns+=result.profile.compiled_ns; profile->blocks+=result.profile.blocks;
            ++profile->jobs; profile->job_ns+=result.profile.job_ns;
            profile->longest_job_ns=std::max(profile->longest_job_ns,result.profile.job_ns);
            profile->execution.merge(result.profile.execution);
        }
        if (job_status_==Stop::budget) job_status_=result.status;
        if (job_status_!=Stop::budget) return job_status_;
        binding_=std::move(result.binding); state=std::move(result.state); completed_=result.completed;
        return Stop::budget;
    }
    Stop submit_sequence(unsigned unit,std::span<const NativeSubmissionStep> steps) {
        last_sequence={};
        if (unit!=binding_.unit() || steps.empty() || steps.size()>NativePacketBatch::maximum_descriptors) return Stop::unsupported_vector;
        if (shared_ && shared_->active) return Stop::vector_pending;
        if (busy_) return Stop::vector_pending;
        if (job_status_!=Stop::budget) return job_status_;
        if (asynchronous_) return begin_sequence(steps);
        // Reuse the ordinary compiled launch path against private candidate
        // state. Shared EE state remains private until every step succeeds.
        // A streaming asynchronous owner can consume completed launch packets;
        // the synchronous transaction retains whole-sequence publication.
        auto candidate_binding=binding_;
        VectorStorage candidate_shared{};
        if (shared_) candidate_shared=*shared_;
        NativeVuLaunch candidate(candidate_binding,block_budget_,shared_ ? &candidate_shared : nullptr);
        candidate.profile=profile;
        candidate.state=state; candidate.completed_=completed_;
        SequenceOutput output;
        candidate.sequence_output_=&output;
        for (const auto& step:steps) {
            candidate.block_budget_=std::min(block_budget_,sequence_budget_-last_sequence.blocks);
            if (step.launch && !candidate.block_budget_) {
                last_result={vu::RunStatus::budget_exhausted,0};
                return Stop::unsupported_vector;
            }
            const auto request=step.launch ? &*step.launch : nullptr;
            const auto result=step.uploads.empty() ? candidate.submit(unit,step.data,request,step.drawing) :
                candidate.submit_uploaded(unit,step.uploads,step.data,request,step.drawing);
            last_result=candidate.last_result; last_entry=candidate.last_entry; last_next_pair=candidate.last_next_pair;
            if (step.launch) {
                ++last_sequence.launches; last_sequence.blocks+=last_result.blocks;
                last_sequence.largest_launch=std::max(last_sequence.largest_launch,last_result.blocks);
            }
            if (result!=Stop::budget) { failed_state=std::move(candidate.failed_state); return result; }
            if (stream_ && !stream_->publish(output)) return Stop::unsupported_vector;
        }
        if (!accept_packets(output)) return Stop::unsupported_vector;
        binding_=std::move(candidate_binding); state=std::move(candidate.state); completed_=candidate.completed_;
        if (shared_) *shared_=candidate_shared;
        return Stop::budget;
    }
    Stop submit_uploaded(unsigned unit,std::span<const NativeUploadView> uploads,
                         std::span<const NativeVectorDataView> data,const NativeLaunchView* request,
                         std::span<const graphics::Packet> prefix={}) {
        if (asynchronous_) {
            NativeSubmissionStep step{{uploads.begin(),uploads.end()},{data.begin(),data.end()},{prefix.begin(),prefix.end()},{}};
            if (request) step.launch=*request;
            return submit_sequence(unit,std::span(&step,1));
        }
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
        if (asynchronous_) return submit_uploaded(unit,{},data,request,prefix);
        if (unit!=binding_.unit()) return Stop::unsupported_vector;
        if (shared_ && shared_->active) return Stop::vector_pending;
        if (request && request->continuation && !completed_) return Stop::unsupported_vector;
        // A sequence already owns private state and publishes only after every
        // step succeeds. Avoid a second state transaction for each nested launch.
        std::optional<vu::State> transaction;
        if (!sequence_output_) transaction.emplace(state);
        auto& next=transaction ? *transaction : state;
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
            const auto run_compiled=[&](bool resume,std::uint64_t budget) {
                const auto began=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto result=resume ? binding_.resume(next,budget,profile ? &profile->execution : nullptr) :
                    binding_.launch(next,last_entry,budget,profile ? &profile->execution : nullptr);
                if (profile) {
                    profile->compiled_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
                    profile->blocks+=result.blocks;
                }
                return result;
            };
            last_result=run_compiled(false,block_budget_);
            last_next_pair=next.next_pair;
            std::uint64_t blocks=last_result.blocks;
            while (last_result.status==vu::RunStatus::graphics_pending) {
                if (!sequence_output_ && !graphics.submit) return Stop::unsupported_vector;
                vu::GraphicsPacket packet;
                try { packet=graphics::collect_packet(next.data,next.graphics_address); }
                catch (const std::invalid_argument&) { return Stop::unsupported_vector; }
                if (packet.size()>maximum_quadwords-quadwords) return Stop::unsupported_vector;
                quadwords+=packet.size(); packets.push_back(std::move(packet));
                next.graphics_pending=0;
                last_result=run_compiled(true,block_budget_-blocks);
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
            bool accepted=true;
            if (sequence_output_) {
                auto& output=*sequence_output_;
                if (quadwords>NativePacketBatch::maximum_bytes/16-output.quadwords) accepted=false;
                else {
                    output.quadwords+=quadwords;
                    for (auto& packet:packets) output.packets.push_back(std::move(packet));
                }
            } else accepted=graphics.submit && graphics.submit(graphics.context,packets);
            if (!accepted) {
                last_result.status=vu::RunStatus::graphics_pending;
                return Stop::unsupported_vector;
            }
        }
        if (transaction) state=std::move(*transaction);
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
        packets.sequence_pending=[](void* raw,unsigned) { return static_cast<NativeVuLaunch*>(raw)->pending(); };
        packets.poll_sequence=[](void* raw,unsigned) { return static_cast<NativeVuLaunch*>(raw)->poll(); };
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
            unit() ? 16384U : 4096U,shared_ ? &shared_->active : &busy_};
    }
private:
    struct SequenceOutput { std::vector<graphics::Packet> packets; std::size_t quadwords=0; };
    struct GraphicsStream {
        std::mutex mutex;
        SequenceOutput ready;
        std::size_t total_words=0;
        bool publish(SequenceOutput& output) {
            if (output.packets.empty()) return true;
            std::lock_guard lock(mutex);
            if (output.quadwords>NativePacketBatch::maximum_bytes/16-total_words) return false;
            total_words+=output.quadwords;
            ready.quadwords+=output.quadwords;
            for (auto& packet:output.packets) ready.packets.push_back(std::move(packet));
            output.packets.clear(); output.quadwords=0;
            return true;
        }
        SequenceOutput take() {
            std::lock_guard lock(mutex);
            SequenceOutput output;
            std::swap(output,ready);
            return output;
        }
    };
    bool accept_packets(SequenceOutput& output) {
        if (output.packets.empty()) return true;
        if (!sequence_output_) return graphics.submit && graphics.submit(graphics.context,output.packets);
        if (output.quadwords>NativePacketBatch::maximum_bytes/16-sequence_output_->quadwords) return false;
        sequence_output_->quadwords+=output.quadwords;
        for (auto& packet:output.packets) sequence_output_->packets.push_back(std::move(packet));
        return true;
    }
    struct OwnedStep {
        std::vector<std::vector<std::uint8_t>> uploads;
        std::vector<std::vector<NativeVectorWrite>> writes;
        NativeSubmissionStep view;
        explicit OwnedStep(const NativeSubmissionStep& source) {
            view.launch=source.launch; view.drawing=source.drawing;
            uploads.reserve(source.uploads.size()); view.uploads.reserve(source.uploads.size());
            for (const auto& upload:source.uploads) {
                uploads.emplace_back(upload.bytes.begin(),upload.bytes.end());
                view.uploads.push_back({upload.destination,uploads.back()});
            }
            writes.reserve(source.data.size()); view.data.reserve(source.data.size());
            for (const auto& data:source.data) {
                if (!data.planned.empty()) writes.emplace_back(data.planned.begin(),data.planned.end());
                else {
                    const auto plan=plan_vector_data(1024,data);
                    if (!plan) throw std::invalid_argument("Invalid native vector job input");
                    writes.push_back(plan->writes);
                }
                NativeVectorDataView owned{}; owned.planned=writes.back();
                view.data.push_back(owned);
            }
        }
    };
    struct JobResult {
        vu::Binding binding;
        vu::State state;
        Profile profile;
        SequenceWork sequence;
        vu::RunResult last_result;
        unsigned entry,next;
        bool completed;
        Stop status;
        std::unique_ptr<vu::State> failed;
    };
    Stop begin_sequence(std::span<const NativeSubmissionStep> steps) {
        std::vector<OwnedStep> owned;
        owned.reserve(steps.size());
        try { for (const auto& step:steps) owned.emplace_back(step); }
        catch (const std::invalid_argument&) { return Stop::invalid_state; }
        // The worker sees only owned input, immutable compiled code and private
        // candidate state. Completed packets cross an ordered, bounded queue.
        stream_=std::make_shared<GraphicsStream>();
        job_=std::async(std::launch::async,[owned=std::move(owned),binding=binding_,initial=state,stream=stream_,
            completed=completed_,block_budget=block_budget_,sequence_budget=sequence_budget_,profiling=profile!=nullptr]() mutable {
            const auto began=profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            NativeVuLaunch worker(binding,block_budget,nullptr,sequence_budget);
            worker.state=std::move(initial); worker.completed_=completed;
            SequenceOutput output; Profile profile;
            worker.sequence_output_=&output; worker.stream_=std::move(stream); worker.profile=profiling ? &profile : nullptr;
            std::vector<NativeSubmissionStep> views; views.reserve(owned.size());
            for (const auto& step:owned) views.push_back(step.view);
            const auto status=worker.submit_sequence(1,views);
            if (profiling) profile.job_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
            return JobResult{std::move(binding),std::move(worker.state),profile,worker.last_sequence,
                worker.last_result,worker.last_entry,worker.last_next_pair,worker.completed_,status,std::move(worker.failed_state)};
        });
        busy_=true;
        return Stop::budget;
    }
    SequenceOutput* sequence_output_=nullptr;
    bool asynchronous_=false,busy_=false;
    Stop job_status_=Stop::budget;
    std::shared_ptr<GraphicsStream> stream_;
    std::future<JobResult> job_;
    vu::Binding& binding_;
    std::uint64_t block_budget_;
    std::uint64_t sequence_budget_;
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
        packets.sequence_pending=[](void* raw,unsigned unit) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            return unit<self.owners_.size() && self.owners_[unit] && self.owners_[unit]->pending();
        };
        packets.poll_sequence=[](void* raw,unsigned unit) {
            auto& self=*static_cast<NativeVuSubmissions*>(raw);
            return unit<self.owners_.size() && self.owners_[unit] ? self.owners_[unit]->poll() : Stop::unsupported_vector;
        };
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
