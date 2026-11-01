#pragma once
#include <cstdint>
#include <optional>

namespace nt::iop {
enum class NativeWaitKind : std::uint32_t { semaphore=3,event=4,mailbox=5,fixed_pool=7,module=0x100,rpc=0x101 };
class NativeWaitQueue {
public:
    virtual ~NativeWaitQueue()=default;
    virtual bool cancel_wait(std::uint32_t object,std::uint32_t thread)=0;
};
// Synchronization services retain opaque continuation IDs, never references to CPU frames.
class NativeThreadWaits {
public:
    virtual ~NativeThreadWaits()=default;
    virtual std::optional<std::uint32_t> current_waiter() const=0;
    virtual void suspend_wait(std::uint32_t thread,std::uint32_t object,NativeWaitKind kind,NativeWaitQueue& owner)=0;
    virtual std::uint32_t wait_priority(std::uint32_t thread) const=0;
    virtual void resume_wait(std::uint32_t thread,std::int32_t result)=0;
};
}
