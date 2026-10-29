#pragma once
#include <nt/iop_native_interrupts.hpp>
#include <nt/iop_native_waits.hpp>
#include <nt/iop_native_heap.hpp>
#include <cstring>
#include <chrono>
#include <functional>
#include <deque>
#include <map>

namespace nt::iop {
// Cooperative continuation scheduler with native stack ownership. Priority chooses
// the next ready continuation; asynchronous preemption remains unsupported.
class NativeThreadServices final : public NativeServices, public NativeThreadWaits {
public:
    using Clock=std::function<std::chrono::steady_clock::time_point()>;
    explicit NativeThreadServices(NativeInterruptServices& interrupts,NativeHeapServices* heap=nullptr,Clock clock={})
        : interrupts_(interrupts), heap_(heap),clock_(std::move(clock)) {
        if (!clock_) clock_=[] { return std::chrono::steady_clock::now(); };
        idle_.pc=idle_address;
    }
    std::optional<std::chrono::nanoseconds> delay() const {
        std::optional<std::chrono::steady_clock::time_point> next;
        for (const auto& [id,thread] : threads_)
            if (thread.status==Status::delayed && (!next || thread.deadline<*next)) next=thread.deadline;
        if (!next) return {};
        const auto now=clock_();
        return *next<=now ? std::chrono::nanoseconds::zero() : std::chrono::duration_cast<std::chrono::nanoseconds>(*next-now);
    }
    std::uint32_t admit(const Cpu& state) {
        const auto entry=ram_offset(state.pc,4,0x200000);
        if (!entry || !nt_iop_native_instruction(*entry) || state.interrupt_depth)
            throw std::invalid_argument("Native thread requires a compiled foreground entry");
        if (threads_.size()>=4096 || next_id_>0x7fffffffU) throw std::length_error("Native thread limit exceeded");
        const auto id=next_id_++;
        Thread thread{state}; thread.entry=state.pc; thread.gp=state.registers[28];
        threads_.emplace(id,thread); ready_.push_back(id); return id;
    }
    bool contains(std::uint32_t id) const { return threads_.contains(id); }
    const Cpu& state(std::uint32_t id) const { return threads_.at(id).cpu; }
    std::uint64_t executed() const { return executed_; }
    std::optional<std::uint32_t> current_waiter() const override {
        if (!current_ || threads_.at(*current_).cpu.interrupt_depth) return {};
        return current_;
    }
    void suspend_wait(std::uint32_t id,std::uint32_t object,NativeWaitKind kind) override {
        if (current_waiter()!=id) throw std::logic_error("Synchronization wait has no active continuation");
        threads_.at(id).status=Status::waiting; threads_.at(id).wait_id=object; threads_.at(id).wait_kind=kind;
    }
    std::uint32_t wait_priority(std::uint32_t id) const override { return threads_.at(id).priority; }
    void resume_wait(std::uint32_t id,std::int32_t result) override {
        auto& thread=threads_.at(id);
        if (thread.status!=Status::waiting) throw std::logic_error("Synchronization completion has no waiting continuation");
        ready_.push_back(id);
        thread.cpu.registers[2]=static_cast<std::uint32_t>(result);
        thread.status=Status::ready; thread.wait_id=0;
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::thbase) return {};
        const auto op=request.selector;
        if (!((op>=4 && op<=9) || op==14 || op==20 || op==22 || op==33 || (op>=24 && op<=28))) return {};
        const auto returned=[](std::int32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        const bool interrupt_call=op==26 || op==28;
        if (interrupt_call!=request.interrupt_context) return returned(-100);
        if (op==4) return create(request,ram);
        if (op==14) return change_priority(request);
        if (op==22) return refer_status(request,ram);
        if (op==33) {
            if (!current_) return {};
            const auto now=clock_();
            const auto duration=std::chrono::microseconds(std::max(request.arguments[0],100U));
            if (now>std::chrono::steady_clock::time_point::max()-duration) return {};
            auto& thread=threads_.at(*current_);
            thread.deadline=now+duration; thread.wait_id=request.arguments[0]; thread.status=Status::delayed;
            return {NativeServiceStatus::yielded};
        }
        if (op>=5 && op<=7) {
            const auto id=request.arguments[0];
            if (!id) return returned(-406);
            const auto found=threads_.find(id);
            if (found==threads_.end() || !found->second.stack) return returned(-407);
            auto& thread=found->second;
            if (thread.status!=Status::finished) return returned(-414);
            if (op==5) {
                const auto released=release_stack(thread.stack,thread.stack_size,thread.attributes,ram);
                if (released.status!=NativeServiceStatus::returned) return released;
                threads_.erase(found); return returned();
            }
            Cpu next; next.pc=thread.entry;
            next.registers[4]=request.arguments[1]; next.registers[28]=thread.gp;
            auto stack_top=thread.stack+thread.stack_size;
            std::optional<std::uint32_t> source;
            std::uint32_t count=0;
            if (op==7) {
                count=request.arguments[1];
                // Keep a usable stack below the copied block. Unsupported sizes
                // leave the dormant worker unchanged rather than underflowing SP.
                if (count>thread.stack_size-0x130) return {};
                stack_top-=(count+15)&~15U;
                next.registers[5]=stack_top;
                if (count && request.arguments[2]) {
                    source=ram_offset(request.arguments[2],1,ram.size());
                    if (!source || count>ram.size()-*source || count>0x200000-*source)
                        return {NativeServiceStatus::address_fault,0,request.arguments[2]};
                    const auto checked=writable(ram,stack_top,(count+3)&~3U);
                    if (checked.status!=NativeServiceStatus::returned) return checked;
                }
            }
            next.registers[29]=stack_top-16; next.registers[30]=next.registers[29];
            next.registers[31]=thread_return;
            ready_.push_back(id);
            if (source) std::memmove(ram.data()+stack_top,ram.data()+*source,count);
            thread.cpu=next; thread.wakeups=0; thread.wait_id=0; thread.priority=thread.initial_priority; thread.status=Status::ready;
            return current_ ? NativeServiceReply{NativeServiceStatus::yielded} : returned();
        }
        if (op==8 || op==9) {
            if (!current_) return {};
            auto& thread=threads_.at(*current_);
            if (op==9) {
                // Host-admitted entries do not own a stack allocation to delete.
                if (!thread.stack) return {};
                const auto checked=writable(ram,thread.stack,thread.stack_size);
                if (checked.status!=NativeServiceStatus::returned) return checked;
            }
            thread.exit_requested=true; thread.delete_on_exit=op==9;
            return {NativeServiceStatus::yielded};
        }
        if (op==20) return current_ ? returned(static_cast<std::int32_t>(*current_)) : NativeServiceReply{};
        if (op==24) {
            if (!current_) return {};
            auto& thread=threads_.at(*current_);
            if (thread.wakeups) { --thread.wakeups; return returned(); }
            thread.status=Status::sleeping;
            return {NativeServiceStatus::yielded};
        }
        auto id=request.arguments[0];
        if (op==27 && !id && current_) id=*current_;
        if (!id || (op==25 && current_ && id==*current_)) return returned(-406);
        const auto found=threads_.find(id);
        if (found==threads_.end()) return returned(-407);
        auto& thread=found->second;
        if (op==27 || op==28) { const auto count=thread.wakeups; thread.wakeups=0; return returned(static_cast<std::int32_t>(count)); }
        if (thread.status==Status::finished) return returned(-413);
        if (thread.status==Status::sleeping) { ready_.push_back(id); thread.status=Status::ready; }
        else {
            if (thread.wakeups==0x7fffffffU) return {}; // Do not overflow the signed ABI count.
            ++thread.wakeups;
        }
        return returned();
    }
    NativeResult run(NativeEntry entry,std::span<std::uint8_t> ram,std::uint64_t budget,NativeServices& services) {
        while (budget) {
            if (interrupts_.delivery_enabled()) {
                const auto now=clock_();
                for (auto& [id,thread] : threads_) if (thread.status==Status::delayed && thread.deadline<=now) {
                    ready_.push_back(id); thread.status=Status::ready; thread.wait_id=0;
                }
            }
            if (!current_ && !idle_active_ && !ready_.empty()) {
                const auto next=std::min_element(ready_.begin(),ready_.end(),[&](auto left,auto right) {
                    return threads_.at(left).priority<threads_.at(right).priority;
                });
                current_=*next; ready_.erase(next);
            }
            Cpu& cpu=current_ ? threads_.at(*current_).cpu : idle_;
            const auto before=cpu.executed;
            const auto stop=interrupts_.run(entry,cpu,ram,budget,services);
            const auto used=cpu.executed-before; budget-=used; executed_+=used;
            if (!current_) {
                idle_active_=stop.reason==NativeStop::budget;
                if (stop.reason==NativeStop::unmapped_code && cpu.pc==idle_address && !cpu.interrupt_depth) {
                    if (!ready_.empty()) continue;
                    return {NativeStop::thread_wait,idle_address};
                }
                return stop;
            }
            auto& thread=threads_.at(*current_);
            if (stop.reason==NativeStop::thread_yield && thread.exit_requested) {
                thread.cpu.pc=thread_return; thread.exit_requested=false;
                thread.status=Status::finished;
                const auto id=*current_; current_.reset();
                if (thread.delete_on_exit) {
                    const auto released=release_stack(thread.stack,thread.stack_size,thread.attributes,ram);
                    if (released.status!=NativeServiceStatus::returned)
                        throw std::logic_error("Validated native exit stack changed during retirement");
                    threads_.erase(id);
                }
                return {NativeStop::unmapped_code,thread_return};
            }
            if (stop.reason==NativeStop::thread_yield && thread.status==Status::ready) {
                ready_.push_back(*current_); current_.reset(); continue;
            }
            if (stop.reason==NativeStop::thread_yield && (thread.status==Status::sleeping || thread.status==Status::waiting || thread.status==Status::delayed)) {
                current_.reset(); continue;
            }
            if (stop.reason==NativeStop::unmapped_code && cpu.pc==thread_return && !cpu.delay_slot && !cpu.interrupt_depth) {
                thread.status=Status::finished; current_.reset();
                // Report explicit completion to the caller before running another entry.
                return stop;
            }
            return stop;
        }
        return {NativeStop::budget,current_ ? threads_.at(*current_).cpu.pc : idle_.pc};
    }
private:
    NativeServiceReply refer_status(const NativeServiceRequest& request,std::span<std::uint8_t> ram) const {
        auto id=request.arguments[0];
        if (!id) {
            if (!current_) return {};
            id=*current_;
        }
        const auto found=threads_.find(id);
        if (found==threads_.end()) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-407)};
        const auto address=request.arguments[1];
        const auto checked=writable(ram,address,68);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto& thread=found->second;
        const auto status=thread.status==Status::finished ? 0x10U :
            thread.status==Status::sleeping || thread.status==Status::waiting || thread.status==Status::delayed ? 4U : current_==id ? 1U : 2U;
        const auto wait_type=thread.status==Status::sleeping ? 1U : thread.status==Status::waiting ? static_cast<std::uint32_t>(thread.wait_kind) : thread.status==Status::delayed ? 2U : 0U;
        // The foreground status ABI does not expose a native register-frame
        // pointer. Its interrupt-only context and reserved fields remain zero.
        const std::array<std::uint32_t,17> fields{thread.attributes,thread.option,status,
            thread.entry,thread.stack,thread.stack_size,thread.gp,thread.initial_priority,
            thread.priority,wait_type,thread.wait_id,thread.wakeups,0,0,0,0,0};
        const auto destination=*ram_offset(address,4,ram.size());
        for (unsigned index=0;index<fields.size();++index) store_native_word(ram,destination+index*4,fields[index]);
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply change_priority(const NativeServiceRequest& request) {
        auto id=request.arguments[0];
        if (!id) {
            if (!current_) return {};
            id=*current_;
        }
        const auto found=threads_.find(id);
        if (found==threads_.end()) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-407)};
        auto& thread=found->second;
        if (thread.status==Status::finished) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-413)};
        auto priority=request.arguments[1];
        if (!priority) {
            if (!current_) return {};
            priority=threads_.at(*current_).priority;
        }
        if (priority<1 || priority>126) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(-403)};
        thread.priority=priority;
        // A changed ready thread joins the back of its new priority group. Keep
        // the single ready list, whose stable minimum selection defines FIFO ties.
        const auto ready=std::find(ready_.begin(),ready_.end(),id);
        if (ready!=ready_.end()) std::rotate(ready,ready+1,ready_.end());
        if (current_) {
            const auto current_priority=threads_.at(*current_).priority;
            const bool reschedule=std::any_of(ready_.begin(),ready_.end(),[&](auto other) {
                const auto other_priority=threads_.at(other).priority;
                return other_priority<current_priority || (id==*current_ && other_priority==current_priority);
            });
            if (reschedule) return {NativeServiceStatus::yielded};
        }
        return {NativeServiceStatus::returned};
    }
    static NativeServiceReply writable(std::span<std::uint8_t> ram,std::uint32_t address,std::uint32_t size) {
        const auto offset=ram_offset(address,4,ram.size());
        if (!offset || size>ram.size()-*offset || size>0x200000-*offset)
            return {NativeServiceStatus::address_fault,0,address};
        for (auto at=*offset;at<*offset+size;at+=4)
            if (nt_iop_native_code_word(at)) return {NativeServiceStatus::code_write,0,address};
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply release_stack(std::uint32_t stack,std::uint32_t size,std::uint32_t attributes,std::span<std::uint8_t> ram) {
        const auto checked=writable(ram,stack,size);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (attributes&0x200000) std::fill_n(ram.begin()+stack,size,0);
        return heap_->invoke({5,{stack,0,0,0},NativeServiceKind::sysmem},ram);
    }
    NativeServiceReply create(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        if (!heap_) return {};
        const auto returned=[](std::int32_t value) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        const auto pointer=request.arguments[0];
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || ram.size()-*offset<20 || 0x200000-*offset<20)
            return {NativeServiceStatus::address_fault,0,pointer};
        const auto attributes=load_native_word(ram,*offset),entry=load_native_word(ram,*offset+8);
        const auto size=load_native_word(ram,*offset+12),priority=load_native_word(ram,*offset+16);
        if (attributes&~0x03300008U) return returned(-401);
        if (priority>126) return returned(-403);
        if (entry&3) return returned(-402);
        const auto code=ram_offset(entry,4,0x200000);
        if (!code || !nt_iop_native_instruction(*code)) return {NativeServiceStatus::unmapped_code,0,entry};
        if (size<0x130) return returned(-404);
        if (size>0x200000 || threads_.size()>=4096 || next_id_>0x7fffffffU) return returned(-400);
        const auto checked=writable(ram,pointer+12,4);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        const auto aligned=(size+255)&~255U;
        const auto allocation=heap_->invoke({4,{1,aligned,0,0},NativeServiceKind::sysmem},ram);
        if (allocation.status!=NativeServiceStatus::returned) return allocation;
        if (!allocation.value) return returned(-400);
        const auto stack=allocation.value;
        const auto stack_check=writable(ram,stack,aligned);
        if (stack_check.status!=NativeServiceStatus::returned) {
            heap_->invoke({5,{stack,0,0,0},NativeServiceKind::sysmem},ram); return stack_check;
        }
        Thread thread; thread.status=Status::finished; thread.entry=entry; thread.gp=request.global_pointer;
        thread.stack=stack; thread.stack_size=aligned; thread.attributes=attributes; thread.priority=priority; thread.initial_priority=priority;
        thread.option=load_native_word(ram,*offset+4);
        try { threads_.emplace(next_id_,thread); }
        catch (...) { heap_->invoke({5,{stack,0,0,0},NativeServiceKind::sysmem},ram); throw; }
        if (!(attributes&0x100000)) std::fill_n(ram.begin()+stack,aligned-0x30,0xff);
        store_native_word(ram,*offset+12,aligned);
        return returned(static_cast<std::int32_t>(next_id_++));
    }
    enum class Status { ready, sleeping, waiting, delayed, finished };
    struct Thread {
        Cpu cpu;
        std::chrono::steady_clock::time_point deadline{};
        Status status=Status::ready; std::uint32_t wakeups=0;
        std::uint32_t entry=0,gp=0,stack=0,stack_size=0,attributes=0,priority=8;
        NativeWaitKind wait_kind=NativeWaitKind::event;
        std::uint32_t option=0,initial_priority=8,wait_id=0;
        bool exit_requested=false,delete_on_exit=false;
    };
    NativeInterruptServices& interrupts_;
    NativeHeapServices* heap_;
    Clock clock_;
    std::map<std::uint32_t,Thread> threads_;
    std::deque<std::uint32_t> ready_;
    std::optional<std::uint32_t> current_;
    std::uint32_t next_id_=1;
    std::uint64_t executed_=0;
    Cpu idle_{};
    bool idle_active_=false;
    static constexpr std::uint32_t idle_address=0xfffffff4,thread_return=0xfffffffc;
};
}
