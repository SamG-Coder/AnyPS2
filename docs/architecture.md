# Architecture

Current policy: LLVM is the only supported conversion backend. Older sections
below describe historical diagnostic C++/interpreter milestones; those are not
supported output paths. The EE C++ generator and its execution test have been removed. IOP milestones now run through iop-llvm-execution and iop-llvm-project.


## Contract and layers

The target pipeline lifts PS2 programs into typed AnyPS2 IR, modernizes supported
operations, lowers to LLVM and compiles ahead of time. Native C++ services provide
Vulkan rendering and other PC facilities. There is no production interpreter/JIT
fallback. LLVM is the only supported game-code backend.

The initial IOP IR separates decoding and reachable-code discovery (`ir.cpp`)
from LLVM emission (`llvm.cpp`). It records semantic operations, register
operands, widths, immediates and control-flow targets without source strings or
raw instruction words. It is still a low-level IOP representation, not recovered
high-level game logic or SSA. Delay slots and load ordering remain backend
responsibilities. Function recovery and native-service transformations remain
unfinished; EE/VU do not yet share this representation.

IOP discovery starts from the module entry, explicit extra roots and functions
in validated export-table candidates. Export parsing is shared in the ELF layer
with the diagnostic library registry; the compiler does not link the IOP runtime.
The inventory scans aligned initialized words for the export signature and fails
on malformed candidates. This is conservative discovery, not proof of runtime
registration or exhaustive recovery of callbacks. A converted entry path alone
does not establish that all callable module functions are supported.

The LLVM backend emits textual IR with explicit wrapping arithmetic, load delays,
aligned/bounded RAM access and precise fault stops. It uses a small C ABI with
fixed-width words, avoiding C++ struct layout assumptions. Clang compiles the IR
at build time; the executable does not link LLVM or parse instructions. The
initial build target is Windows x86-64/MSVC. Sequential instructions and branch
delay-slot entry use direct LLVM edges instead of returning to PC dispatch.
Entry/resumption and completed delay slots still dispatch their current or saved
target. Every instruction checks the remaining slice before execution, including
along direct edges. Execution still uses explicit state memory; recovered functions
and further block optimization remain future work, with no measured speed claim.
Both output backends share
the same synthetic execution regression. LLVM is the only standalone CLI backend.

LLVM execution functions cover fixed 1 KiB address ranges, with at most 256
translated instructions per function. Direct edges stay within a range; crossing
it returns to a nonrecursive module dispatcher with the same remaining budget and
pending control/load state. Execution functions are marked noinline so ordinary
optimization does not merge the module back into one function. Code-write checks
also use per-range membership tables. Analysis is bounded to 65,536 reachable
instructions per module. These ranges are compiler
partitions, not recovered game functions, and imply no measured speed improvement.

Indirect register calls and jumps are accepted when forward value-set analysis
proves a finite set of targets across incoming paths. Each register retains up to
16 values; merges take the union, and excessive growth widens to unknown instead
of dropping destinations. External roots start unknown. Load results, service
effects and call-continuation registers are forgotten conservatively. Immediate
construction and selected integer operations propagate bounded sets without
reading mutable data as function pointers. Every possible reachable callee is
converted ahead of time. Delay-slot effects follow target capture. Backends guard
the physical destination against the proven set before retirement, including at
budget boundaries and trapping delay slots. Unknown sets fail conversion. This
supports branch-selected callbacks; memory-based callback recovery and general
jump tables remain unfinished. No runtime instruction decoding is introduced.

Compiled SYSCALL sites return a typed service request without retiring the
instruction. `run_native` can route that boundary to an explicitly supplied
`NativeServices` provider and resume compiled code after a successful return.
The return-value contract carries v0's selector and a0-a3, commits pending loads
with the result write taking precedence, and charges the syscall to the execution
budget. Missing/unsupported services leave the continuation unchanged; providers
must validate requests before mutating RAM. Delay-slot services, context switches
and callbacks are not supported by this contract. No PS2 syscall provider is
installed by default, and the standalone host stops on these requests. The
synthetic test provider proves routing only, not PS2 kernel-service support.

Native library bindings cover `sysclib` major version 1, ordinals 12-14 (`memcpy`,
`memmove`, `memset`). Import recognition creates memory IR operations instead of
translating unbound stubs or linking a guest library. LLVM emits memory intrinsics;
C++ uses standard host memory functions. Both settle pending argument loads,
validate full source/destination ranges before mutation, reject destination overlap
with converted instruction words, return the original destination pointer and
resume at the caller's return address. Zero-length operations access neither
pointer. `memmove` supports overlapping ranges; `memcpy` rejects overlap explicitly,
including overlap through RAM aliases, rather than invoking host undefined behaviour.
A replacement counts as one native operation for execution budgeting; that is not
the replaced PS2 function's instruction count or timing. Other library major versions
and ordinals fail conversion explicitly. Minor revisions share these ordinal bindings;
this does not imply support for other services in that library. Conversion reports
all unresolved imports with library, major/minor version, ordinal and stub address
before emitting output.
Write protection covers both words of each replaced import stub. Protected words
and executable entry points are separate sets: entering the ordinal/delay word
does not invoke a native service or become an executable target. LLVM emits bounded
membership helpers for both sets and uses only executable entries for dispatch.

Native `sysmem` major-version-1 imports 4-10 use an explicit library-service
boundary, distinct from syscall selectors. Generated code returns an import request;
the native driver dispatches it to the supplied provider and resumes at the saved
return address. Pending argument and return-address loads settle at this boundary.
Missing providers and delay-slot imports stop without retiring or mutating memory.
The LLVM ABI uses stop code 9 and its existing detail word: low 16 bits hold
the import ordinal, high 16 bits select sysmem (0), loadcore (1), a compiled
library call (2), ioman (3), or intrman (4).

The standalone host supplies `NativeHeapServices`: host-owned ordered extents over
the data arena, with 256-byte allocation granularity, first/last/fixed-address
allocation, exact-base free, coalescing, total/largest-free queries and block queries
with the public free flag. Allocation failure returns zero; invalid free and block
queries return -1. Requests cannot allocate reserved regions. The host reserves
memory through the module end and from the invocation stack onward; this is a
native arena policy, not a reproduction of a console allocator's internal layout.
Metadata never occupies guest RAM; allocation/free do not initialize or erase the
payload. Internal allocator structures and printf callbacks are unsupported. The
provider is serialized by the native driver; concurrent entry requires future
scheduler synchronization. This does not implement a module loader or native boot.

Native `loadcore` major-version-1 registration/release imports (6/7) operate on
converter-emitted export descriptors. The ELF inventory retains each table address;
project generation emits the library identity, ABI version and compiled function
addresses. Discovery is not registration. The host checks the declaration in RAM
against that metadata before exposing exports. Unknown or modified declarations
return the public illegal-library error; bad RAM pointers fault without retirement.
Duplicate/older registrations and absent releases return their public errors. A
newer minor revision takes precedence; releasing it exposes an older registration.

`NativeLibraryServices::resolve` provides a host lookup of active exports by name,
major ABI and ordinal. A composed provider routes library and allocator requests
without treating syscall numbers as import ordinals. The registry does not rewrite
guest export headers, patch guest instructions, execute loadcore code or compile
new code. Known import descriptors bind when their provider registers. Explicit
loadcore LinkLibraryEntries/UnLinkLibraryEntries (8/9) operate on recorded import
tables in the supplied range, without decoding or modifying instruction bytes.
Relinking validates all selected dependencies before committing any bindings.
Explicitly unlinked clients stay unlinked until a successful link request.
Release refuses providers with bound clients using the public in-use error.

A compiled library import transfers execution to the bound native target while
preserving arguments, v0 and the caller's return address; pending loads settle
before transfer. The next native dispatch executes only code present at build time.
An unregistered or unlinked call stops without retirement. Imports do not create
new executable content. Data-arena descriptors and function addresses are sealed
by conversion; mutating an import table is not a way to add a runtime dependency.

`emit_bundle` accepts separately relocated modules with disjoint data ranges,
collects their export catalog, validates each dependency and compiles every module
through the shared IR/backends. Unknown imports, missing ordinals/major versions,
overlapping modules and unsupported provider code fail before output. Additional
roots must belong to a module. The generated program shares one native dispatch
space; providers must still register before clients can call them. `project_bundle`
and the `--bundle` CLI package explicit base/path pairs into one executable and
initialized data image. Module extents and the host invocation stack cannot overlap.
Each module's entry and global pointer are retained. The host loads all initial data,
then invokes entries in input order using a fresh CPU call frame and a shared arena,
allocator and registry. Module BSS and inter-module gaps start zeroed. An unsupported
stop aborts startup; return values are reported without claiming residency handling.
The single-module command uses the same packaging path. Disc-wide discovery and
layout, dependency-driven startup, module load/unload and active-call lifetime
tracking remain unfinished. QueryLibraryEntryTable (11) reads a bounded library
identity and returns the active compiled export array address, using the same
version selection as host resolution; absent matches return null. Unsupported
loadcore services continue to fail conversion explicitly.

QueryBootMode (12) is served by `NativeBootServices`, whose context is explicitly
supplied by the host. The record ABI is a 16-bit value, 8-bit mode ID, 8-bit payload
word count, then payload words. Construction validates the complete set before
writing to caller-reserved data-arena storage. Queries return stable arena pointers
to the first matching record or null, and reject record lengths that no longer
match their reserved extents. Host pointers and firmware globals are never exposed.
The standalone probe has no reboot context and therefore supplies an empty set;
no console mode or firmware version is invented. RegisterBootMode, reboot-context
construction from EE requests, and CLI boot-record configuration remain unfinished.

Native ioman major-version-1 open/close/read/lseek imports (4/5/6/8) are served by
`NativeDiscServices` when the host mounts an image with `--disc`. The shared
`disc::Files` backend owns read-only handles and is also used by the older FILEIO
RPC adapter; ISO path lookup, descriptor lifetime, seek and read logic are not
duplicated. Guest cdrom paths never name host files. Missing files, invalid
handles, unsupported device prefixes and write modes return negative file errors.
Native reads are bounded to 1 MiB and paths to 255 characters plus a terminator.

The adapter validates the requested data-arena extent and every overlapping code
word before reading, so invalid destinations neither change RAM nor advance the
file cursor. Both backends export the same compiled-code membership predicate;
no instruction bytes are decoded for this check. Zero-length reads touch no data
pointer. Read data is copied only after the ISO backend has completed the read.
Native output compiles only the binary reader, ISO reader and shared file backend,
with no CPU/device/RPC runtime dependency. Missing mounted images leave file imports
unhandled and stop execution explicitly. Writes, device drivers, ioctl and other
ioman services remain unsupported. This data path does not imply game boot.

Native intrman major-version-1 imports 4-9, 17/18 and 23 manage a serialized host
event domain: registration/release, per-cause masks, global disable/enable,
suspend/resume and context queries. Output pointers are validated before any state
change and may not overwrite compiled code. Handler registration accepts only
addresses present in the compiled instruction map, distinct from protected import
metadata. Constant registration arguments extend conversion discovery to handler
code across the bundle, including non-exported handlers in modules analyzed earlier.
Discovery revisits owners with newly found roots until no new callbacks remain;
unaligned addresses, unowned addresses and roots outside executable extents reject
conversion before output. Unknown callback addresses are checked at runtime and never decoded.

The host posts events explicitly. Pending causes coalesce and remain pending while
masked. A deterministic scan selects an enabled handler at a native checkpoint,
after any outstanding delay slot. The dispatcher snapshots the continuation,
uses a separate callback stack, supplies the registered argument, and runs compiled
code through the same service router. Budget exhaustion preserves the callback;
normal return restores the complete foreground state while retaining the total
native-operation count. Nonzero handler returns re-enable the cause; zero leaves
it disabled. Callback faults remain visible and do not resume the foreground.
Global mask state is restored on return. All registers are preserved, supporting
the public modes' permitted clobbers without recreating assembly save frames.

Delivery is cooperative in slices of at most 256 native operations, not PS2 cycle
or hardware timing. Nested delivery, thread preemption callbacks, software-event
enable semantics and device/SIF event producers remain unfinished. The standalone
host supplies the event domain with interrupts initially disabled, but fabricates
no periodic events. Its callback stack starts at 0x1f7000 within the reserved stack
region, below the ordinary invocation stack. Native mode/flag tokens retain API
state only; there are no emulated interrupt-controller or DMA registers.

Native `thevent` major-version-1 ordinals 4-11 and 13/14 use host-owned event
objects with opaque positive handles. The standalone host shares them across
module startup entries. IDs are monotonic, deleted IDs remain invalid, and the
service limits live objects to 4096. Attributes, option and initial/current bits
are retained without exposing a guest kernel object layout. Set ORs bits;
ClearEventFlag applies a keep mask. AND/OR polling returns the full current
pattern, and clear-on-success clears the entire pattern after validating output.
Status writes the five defined words and leaves the two reserved words untouched.

The native service request carries interrupt context from the compiled execution
continuation. Foreground calls reject interrupt context; the interrupt-only set,
clear and status variants require it. Compiled handlers can therefore signal
native events through the same service router. No guest kernel instructions are
executed. WaitEventFlag returns immediately when its condition already holds.
With an explicitly connected scheduler, an unmet wait validates its output, queues
an opaque continuation ID and retires once before suspending that continuation.
Without a scheduler it still stops explicitly without retirement.

Waiters are considered in FIFO order. Signalling first validates all selected
outputs, then writes full result patterns, applies clear-on-success and makes
matching continuations ready. A clearing waiter can leave later waiters blocked.
Single-waiter events reject a second wait/poll while a waiter exists. Status
reports the live queue length; event deletion releases queued continuations with
the deletion error without writing result patterns. WakeupThread does not release
an event wait: it retains a wakeup credit for a subsequent SleepThread instead.
The standalone host shares one scheduler and event domain across startup entries.
Forced release/termination of event waits and asynchronous dispatch are not yet
implemented.

Native GetSystemStatusFlag (thbase ordinal 41) returns a stable handle to an
ordinary multi-waiter event in the shared native event service. Bootstrap creates
it with zero initial bits and option zero. If supplied boot-mode record 4 exists,
it sets the bit selected by that record's low two value bits through the same
event service. Missing boot context leaves current bits zero. No readiness,
firmware version or completed boot stage is fabricated.

Foreground and interrupt queries return the same handle. Compiled event queries,
waits and signals therefore operate on the actual status object. The standalone
host owns this service across startup entries. As with the public fixed-handle
contract, deleting the event does not silently create a replacement on query.
Event allocation is shared with ordinary CreateEventFlag rather than duplicated.

Native `thbase` imports 20 and 24-28 provide thread identity, sleep, foreground/
interrupt wakeup and cancellation of queued wakeups. The cooperative scheduler
owns explicitly admitted compiled continuations. Sleep retires once, saves the
return continuation and yields to the next ready continuation. A wakeup of a
sleeping thread makes it ready; an early wakeup increments a counter consumed by
a later sleep. Cancellation returns and clears that count. Context checks and
invalid/finished thread errors are explicit. No guest instruction fetch occurs.

When all continuations sleep, the scheduler offers an idle continuation to the
native interrupt domain. A posted interrupt can execute a compiled handler and
wake a sleeper. Callback budget exhaustion retains the idle callback until it
finishes, before switching to the awakened thread. With no runnable continuation
or deliverable event, execution returns `thread_wait` without consuming budget.
Total execution accounting includes idle callbacks as well as thread work.

The standalone probe admits each module startup invocation with its own
register/stack context. Compiled CreateThread/StartThread/DeleteThread imports
4-6 additionally manage guest-requested workers. Create validates the public
descriptor and compiled entry, rounds stack size to 256 bytes, uses the shared
allocator and captures the caller GP. The descriptor receives the aligned size.
Default stack fill and clear-on-delete flags are respected; descriptor/stack
writes cannot overlap compiled code. Handles are opaque and never reused.

Start resets the worker continuation, passes its argument and captured GP, gives
it an aligned native invocation stack and makes it ready. Ready selection chooses
the lowest numeric priority, preserving FIFO order among equals. Start is a
cooperative scheduling point. A normal compiled return makes the worker dormant;
it can be restarted or deleted, releasing its stack. The standalone host continues
past worker completion while its startup continuation is still running.

StartThreadArgs (ordinal 7) shares worker startup with StartThread. It reserves a
16-byte-aligned block at the high end of the owned stack, copies the requested
bytes and passes their count/pointer in a0/a1. SP and FP are aligned below the
block with native call space. The converter runtime retains at least 0x130 bytes
below the block; negative/oversized counts stop unsupported before changing the
worker. Invalid source ranges fault before queuing or copying. The checked copy
uses memmove so an overlapping source retains its original bytes. Zero counts
ignore the source; a null source leaves reserved argument bytes unchanged, matching
the public call contract. No guest register-context frame is constructed.

Explicit thbase exits (ordinals 8/9) retire the exit import once and hand control
back to the scheduler without returning into the worker's following code.
ExitThread retains its stack and dormant continuation for restart/deletion.
ExitDeleteThread defers stack clearing/release and handle removal until retirement,
using the same validated release path as DeleteThread. Both reject interrupt
context. Zero remaining budget prevents exit-side effects. Host-admitted startup
entries have externally owned stacks, so ExitDeleteThread remains unsupported for
those entries; their ordinary return or ExitThread is supported.

Worker entries must be in the compiled map. Besides exports and explicit roots,
constant CreateThread descriptor addresses now seed discovery from initialized
module data. The descriptor and entry may use RAM aliases and belong to different
modules. The existing bundle discovery loop includes newly found workers and
revisits owners as needed. Only complete, aligned, initialized descriptors are
read; nonzero initial entries must resolve to executable code or conversion fails
before output. Zero/unavailable initial entries provide no candidate.

Initial descriptor data is a discovery hint, not proof of immutability. Runtime
creation still validates the actual entry against compiled instructions. Descriptors
built or overwritten dynamically may require further analysis or explicit roots;
this change does not recover arbitrary runtime stores or enable fallback decoding.
Priority changes and asynchronous preemption
remain unsupported. This is not complete threadman compatibility. Thread completion is reported at the explicit host return sentinel;
traps and unknown code targets remain visible failures.

Native alarm imports (thbase 34-38) implement GetSystemTime, SetAlarm/iSetAlarm and
CancelAlarm/iCancelAlarm. A host steady clock supplies 64-bit ticks at the default
36,864,000 Hz rate; tests inject the clock, and a native caller can supply another
frequency explicitly. The minimum interval is 100 microseconds converted to ticks.
Short registration intervals update the supplied low word only after pointer and
code-write validation. Duplicate pending callback/userdata pairs and incorrect
foreground/interrupt contexts return the public errors. At most 1024 pending
alarms are retained. Unknown compiled entries and deadline overflow stop explicitly.

Constant alarm callback arguments extend ahead-of-time discovery. Due alarms feed
the same context-preserving callback path as interrupts, with interrupt context,
userdata, a separate stack and budget suspension. Global interrupt masking and
delay-slot deferral apply. A zero callback result removes the alarm; a nonzero
result, clamped to the minimum, advances its previous deadline. Overdue repeated
alarms catch up through bounded compiled execution. An active alarm is absent from
the pending queue, matching cancellation and re-registration behaviour. No timer
registers, guest timer ISR or instruction-based clock are simulated.

When all threads wait and alarm delivery is enabled, the standalone probe sleeps
on the host until the next deadline and then resumes native execution. Its startup
probe limits idle alarm waiting to five seconds per module and retains the existing
operation budget. A masked timer or a wait with no scheduled alarm remains an
explicit wait boundary. This is not a claim of precise console interrupt latency.
Microsecond conversion imports, clock-profile selection from boot records and
asynchronous thread preemption remain unfinished.

Native SIF transfer services (sifman 7/8 and 30-32) replace supported outbound
transfer batches with direct native data copies. The caller must supply an EE
arena and its compiled-code write guard. The standalone IOP probe deliberately
supplies no endpoint, so these services remain unsupported there. This does not
create a second CPU interpreter or pretend that an EE consumer has booted.

The current subset accepts 1-32 descriptors with 16-byte-aligned source,
destination and nonzero size, at most 1 MiB total payload, and attributes 0 or
INT_I. All descriptor ranges, IOP source bytes, EE destination ranges and write
guards are checked before any destination mutation. Source payloads are captured
before writes, then committed in descriptor order. Invalid later descriptors leave
all earlier destinations unchanged. Irregular tails, EE interrupt/tag attributes,
hardware chain parsing and additional transfer modes remain unsupported.

Accepted copies return an opaque positive token and are already complete for
DmaStat; completion callbacks are queued separately. General-handler selection is
resolved when delivery occurs, so resetting it suppresses a queued general call.
Per-transfer callbacks retain their supplied handler/userdata. Completion callback
return values are ignored, as required by their void contract. Queue capacity is
bounded, with space reserved before destination writes. Constant handler arguments
participate in ahead-of-time callback discovery.

A shared callback multiplexer fairly selects pending alarms and SIF completions,
retaining source ownership across compiled budget slices. It reuses the existing
interrupt-context snapshot, masking and stack path. Completion ordering is native
and cooperative; it does not reproduce DMA registers, hardware transfer timing or
exact interrupt grouping. A converted EE command producer/consumer is still required for the actual game
startup path.

Native SIF control (sifman 2, 5/6, 21-27 and 29) uses explicit link coordination.
The EE owner publishes its command-buffer address and status bits. Initialization
stops unsupported until that peer supplies SIFINIT; on success it publishes the
IOP readiness bit, clears the initial sub-buffer address and arms reception.
Repeated initialization preserves established state. CheckInit reports actual
initialization, including zero for a disconnected probe. Status publication ORs
the sender's bits; acknowledgement clears the recipient-observed bits. Command
buffer addresses are shared protocol data, not an emulated MMIO register bank.

An explicit receive call validates a nonempty aligned payload, destination range
and compiled IOP code protection before copying. It then disarms reception and
posts cause 43 to the native interrupt domain. The compiled SIF1 handler observes
committed bytes; masking and callback budgets use the existing delivery path.
SetDChain rearms this native receiver. Invalid delivery leaves data and receive
readiness unchanged; a second delivery cannot overwrite an unacknowledged buffer.
The current receive API handles one aligned batch up to 1 MiB, not DMA tag parsing.

Deinitialization stops reception and outbound submissions, clears queued native
completion callbacks and releases/disables the SIF0 handler. Already committed
bytes remain committed; the historical CheckInit marker and peer status remain.
Cleanup works without a connected peer and is repeatable. An active completion or
interrupt context stops unsupported before any teardown effect. Pending SIF1
notifications still describe data committed before teardown. Restarting a stopped
transport requires a new native session; SetDChain cannot silently reconnect it.

The standalone host constructs disconnected control and outbound services and
coordinates their teardown. It never publishes an EE peer automatically. Explicit
EE interrupt notification, reconnection and actual converted EE command execution
remain unfinished.

Native SIF command registration (sifcmd 8-11) binds explicit user/system tables,
adds compiled callbacks and removes handlers. User entries occupy eight bytes;
system entries occupy twelve, with the third word preserved. Table extents and
indices are bounded; registration validates targets and protected code before
writing. A constant AddCmdHandler function argument seeds native code discovery,
including bundle callbacks, without using a title or fixed game address.

The command receive API accepts an explicit peer packet and caller-reserved RAM
buffer. It validates the header, packet length (16-112 bytes), destination and
current handler pointer before committing anything. Extra-data payloads remain
unsupported. An accepted packet captures its callback and user argument and uses
the shared native interrupt/callback scheduler. Callback entry supplies packet
and user pointers in a0/a1; completion restores foreground state. Packet storage
must remain reserved until completion. One packet may be pending or active, so
later arrivals cannot overwrite a suspended callback's packet. Registered tables
cannot overlap packet storage. Removed handlers consume no callback; a mutated
unknown pointer is rejected rather than executed or interpreted.

The standalone host installs registration support but supplies no peer packets.
The optional SIF1 observer, command exit and complete native RPC remain unfinished. This does not replace all of the disc's SIFCMD
module or resolve its indirect calls; it supplies a native service boundary for
converted clients instead of reproducing that module's dispatcher.

Native outbound command services (sifcmd 12/13 and 28/29) marshal the O32 register and
stack arguments and require an explicit NativeSifCommandEndpoint. Packet sizes
16-112 are supported. Positive extra-data sizes up to 1 MiB copy exactly that
many bytes; nonpositive sizes clear the extra-data header fields. The sender
preserves the option word, fills size/destination/command fields and accounts for
source buffers overlapping the updated header. Regular SendCmd is unsupported
in interrupt context; iSendCmd supports that context. SendCmdIntr/iSendCmdIntr
also read the callback and user argument from the O32 stack. A nonnull callback
must already belong to compiled code; unknown targets reject the send without
changing either side. Dynamically constructed stack callback arguments may need
additional conversion roots; the sender never interprets an unknown target.

The endpoint owns a supplied EE data arena, code-write guard and published
command-buffer address. It validates both destination extents and snapshots
source data before committing writes. Extra data becomes visible before the
command packet and notification. A peer consumes an owned packet via take();
there is no fabricated DMA engine or instruction execution. One pending packet
provides backpressure: a busy send returns zero and preserves peer data, while
still applying the caller's header update. Other validation failures leave both
sides unchanged. Successful submissions return increasing positive tokens from the same allocator
as raw SIF transfers. Both paths share one commit helper: validate callbacks and
reserve queue storage before a nonthrowing data commit, then publish completion.
The general SIF callback and optional per-command callback run through the native
interrupt domain; void return values never repeat them. SIF status reports these
synchronous copies complete. Shared teardown cancels queued completions and
prevents new command submissions while preserving already committed peer data
and notifications. A converted EE packet consumer remains unfinished; the
standalone probe deliberately has no endpoint. This transport
milestone is not evidence of EE startup or RPC completion.

Native command session services implement InitCmd and Get/SetSreg (sifcmd 4,
6/7). The 32 software values are protocol state, not emulated hardware registers.
Local and peer writes share the same bounded array. InitCmd requires an explicit
peer and an initialized, live SIF link. It uses the ordinary shared system-status
event: publish CMDINIT and yield until bit 0x100 is signalled. No scheduler means
no suspended continuation and no fabricated readiness publication. Repeated calls
observe the existing event state without resetting it.

The inbound router delegates built-in control packets to receive_control. Change
Address publishes a validated peer command-buffer address; Set Sreg updates a
bounded software value. Init Command with option zero validates the peer address,
signals 0x100, acknowledges the peer CMDINIT bit and publishes the buffer. A
nonzero option signals 0x800 instead, without satisfying the command-init wait or
changing the address. Validation and event errors must not partially publish the
buffer or acknowledge readiness. A deleted status event is not recreated.

The command session, handler dispatcher and outgoing endpoint remain explicit
native components. NativeSifInbound connects the session and dispatcher through a
single owned 112-byte packet mailbox with an optional payload up to 1 MiB.
Acceptance requires a live link and snapshots both buffers; delivery runs through the callback multiplexer only with interrupts enabled,
no active callback and no branch delay slot. Zero-budget execution cannot deliver
packets. Built-in control IDs 0-2 use the native session unless the caller installs
its own system table; that table then takes precedence. Other packets resolve
registered compiled callbacks at delivery, preserving mutable handler checks.

Deferred validation failures stop native execution with the command ID and fault
address; they remain latched until the owner explicitly discards the packet. No
failure is swallowed as an empty callback queue, and a running callback cannot be
discarded. A sliced user callback retains its mailbox reservation until completion.
The standalone host installs this router with packet storage at the start of its
reserved stack region, but supplies no EE packets. Bootstrap ownership and the
converted EE producer remain unfinished; InitCmd still stops without a peer.

Native RPC server ownership implements SetRpcQueue, RegisterRpc, RemoveRpc and
RemoveRpcQueue (sifcmd 19, 17, 24/25). C++ maps own descriptor identities and queue
membership; bounded vectors preserve registration order. Public queue/server
link fields are mirrored in RAM, preserving address aliases and untouched server
request fields. Registration validates complete descriptor extents, code-write
protection, stack arguments and compiled function pointers before committing.
Constant primary RegisterRpc function arguments seed code discovery; dynamically
supplied and secondary callbacks must already be part of the compiled program.

Duplicate live queues/servers and overlapping owned records stop unsupported
instead of creating cyclic lists. The limits are 128 owned queues and 1024
servers. Removing a queue detaches it from published order while retaining any
server membership for later cleanup. Removing its final server releases native
ownership; empty removed descriptor storage can be reused. Only foreground
registration/removal is supported. Buffer pointers are recorded, not dereferenced
at registration; actual request sizes and buffer validation belong to delivery.
Client operations
remain unfinished, so no successful remote call is implied by registration.

Native server-side RPC binding handles the public Bind command through the
inbound router when no custom system-handler table overrides it. Lookup scans
published queues and their server registrations in insertion order. It reads
current service IDs and buffer pointers from the owned descriptors, revalidates
the selected compiled function and creates a zero-initialized 64-byte RPC End
reply containing the originating packet/client identities and bind result.
Unknown service IDs receive a null server/buffer pair. Detached queues are not
visible to this lookup. No host pointer or uninitialized memory enters the reply.

The router retains the prepared reply while the shared native endpoint is busy,
without dropping the request, inventing success or creating an alarm-driven spin
loop. Retries use the same snapshot even if descriptors change. Once accepted,
the endpoint's existing data/notification/completion ordering applies. Protected
or invalid peer destinations become deferred execution faults; explicit discard
also releases the prepared reply. Missing peers remain unsupported. This is
server-side discovery only, not execution of an RPC function or an EE client.

Native RPC Call admission validates the complete packet and payload lengths, a
registered idle server in a published queue, its compiled function, request and
queue storage, and the registered receive buffer. The packet's extra-data address
must identify that buffer. Payloads cannot overlap queue/server metadata or another
outstanding request's payload. No caller-owned packet bytes are used after the
inbound snapshot. Request metadata and allocation are prepared before worker
wakeup; only bounded nonthrowing writes remain after a successful wake.

Admission fills the public request fields, copies input data and appends a native
FIFO entry. A nonnegative queue thread ID is woken through the existing iWakeupThread
service when the queue is inactive; a missing or invalid worker cannot fabricate
successful admission. Negative thread IDs permit explicit polling. GetNextRequest
(sifcmd 20) updates queue active/start state and returns the original server pointer,
including its alias. Empty retrieval returns zero. The public end field retains
its last value when the queue drains, matching the queue contract.

Queued and retrieved server records remain busy until the execution/reply
path completes them. Reusing those servers or removing their queue stops unsupported
rather than losing a request. Admission alone wakes and retrieves work; it does
not invoke the RPC function or report remote completion.

Native ExecRequest (sifcmd 21) supports command-notified (rmode 1) and polling-mode
(rmode 0) replies. It
revalidates the retrieved server's compiled function and admitted buffer/size,
then enters that function with the request number, input pointer and length.
Registration captures the function's global pointer. The native service bridge
saves the caller continuation and uses a dedicated return sentinel; neither the
sentinel nor callback execution fetches or decodes PS2 instructions. Execution
budgets and native thread suspension retain the service continuation.

On function return, the service validates and snapshots the returned bytes (or no
bytes for a null pointer), builds an RPC End reply and submits through the shared
peer endpoint. A busy peer returns thread_wait at the service return boundary;
resuming retries the saved reply without rerunning the function or consuming a
busy-loop instruction budget. Faults likewise retain the completed function's
continuation. Only accepted reply delivery restores the caller registers/PC,
preserves its aggregate operation count and makes the server reusable. Public
request fields remain available in RAM. The queue's active bit changes on the
next GetNextRequest, as before.

Polling-mode delivery copies the result bytes and then a 64-byte completion
record to the peer's supplied packet address. The completion clears rec_id and
rpc_id and preserves the client and RPC Call subtype. Both writes are validated
and snapshotted before committing through the shared native completion service.
This path neither occupies nor consumes the command mailbox; an unrelated
pending command does not block it. It uses native memory copies rather than DMA
registers or guest instruction execution. Publication assumes the current
serialized peer owner; a concurrent peer would need explicit synchronization.

Pending native calls require a stable Cpu address; the production scheduler owns
stable thread states. Up to 128 contexts may hold RPC continuations. Nested
ExecRequest on the same context and invalid reply modes stop unsupported
before invoking a handler. Client APIs and
converted EE consumption remain unfinished. These limits must not be described
as complete RPC compatibility or a game boot.

Native stdio printf (major-version-1 ordinal 4) formats diagnostic text in C++
and sends it to an explicitly supplied host sink. It supports d/i/u/o/x/X/s/c,
literal percent, ordinary integer flags, numeric or star width/precision, and
32-bit long integers. O32 variadic words come from a1-a3 and then caller stack
slots beginning at SP+16; the service request carries the settled guest SP.
Integer conversion handles the full unsigned range and signed minimum without UB.

Format/string scans are bounded to 4096 bytes, each width/precision to 4096,
argument positions to 127 variadic words and output to 16 KiB. Precision-limited
strings need no terminator beyond the requested bytes. Pointer/stack failures
report address faults; unsupported forms (including floats, long long, percent-n
and pointer formatting) stop explicitly. Formatting validates completely before
calling the sink once, so failures cannot emit a partial diagnostic. The result
is the byte count or -1 on sink failure. The standalone host supplies stdout;
library code does not select files or write to the console itself.

CP0 performance registers are CPU-global bus state, outside thread and interrupt
snapshots. Selector aliases support control and two 32-bit counters with signed
GPR reads. Event 1 counts the diagnostic execution/idle clock when CTE, privilege
and ERL gating permit it. Event 6 remains unchanged because this runtime models
coherent memory, not cache fills or misses. These counters are not hardware timing
measurements. Other active events and overflow exceptions remain explicit stops.

1. Binary/disc/ELF libraries validate sizes and ranges before loading. Import
   accepts ISO9660 with 2048-byte sectors and little-endian MIPS ELF32 ET_EXEC.
   Raw BIN, CHD, interleaving, and multi-extent files are unsupported.
   A separate IOP module loader prepares version-one relocatable IRX files in
   private storage. It bounds allocations to 2 MiB IOP RAM, copies one zero-based
   load segment, zeroes BSS, and applies word, jump and paired high/low relocations.
   Optional module metadata supplies the name, version, global pointer and
   section sizes. The import binder discovers bounded IRX stubs and resolves
   library/version/ordinal requests through a caller-supplied resolver. It patches
   direct guest jumps only after every import resolves, retaining delay slots.
   The separate IOP library registry accepts explicit export registration from
   loaded owners and resolves imports to their guest function addresses. It
   currently requires exact ABI versions and rejects duplicate providers, nonzero
   table modes and targets outside the owner's initialized image. Reset clears
   the registry; it does not rewrite already bound modules. This host registry is
   separate from the disc module manager's live export list; installing a provider
   in one does not silently publish it in the other.
   Version-two IRX and other relocation forms currently stop explicitly. The
   reboot endpoint invokes the archive's memory bootstrap in a private context,
   uses its allocation export to reserve the module-manager region, and installs
   the relocated/bound manager. It populates linked module-info records, allocates
   the reboot archive and a terminated ELF-pointer list, builds public boot
   parameters, and executes the manager entry. Device access and subsequent
   service initialization remain unfinished; returning from an entry alone
   does not establish service readiness.
   A static reboot-archive inventory matches imported library names against
   validated export tables across all supplied IRX modules. It reports missing
   libraries with their consumer, requested version and ordinal. It reuses the
   runtime export parser and operates on private relocated copies; it does not
   register services or infer that they initialized successfully. A separate
   startup planner orders modules by their imported library requirements, keeping
   input order among ready modules. It conservatively requires matching major
   versions, sufficient minor versions and valid ordinals, considering the newest
   available compatible provider. The memory/module managers and native-provider
   registration modules remain first. Genuine dependency cycles and unresolved
   requirements stop explicitly; no consumer is treated as initialized by planning.
   SIF reset diagnostics summarize libraries absent from the archive. A supplied
   reboot archive may depend on base services that AnyPS2 must implement itself;
   reaching the module manager's final loop does not prove those services started.
   A bounded diagnostic IOP execution core now handles integer ALU operations,
   signed/unsigned multiply and divide with HI/LO transfers,
   jumps/conditional branches, link registers and aligned byte/halfword/word RAM
   access, including cached/uncached aliases. Branch and load-delay state survives
   budget suspension. HI/LO results include division by zero and signed overflow;
   multiply/divide pipeline timing is not modeled. CP0 status transfers preserve
   the read delay. Isolated
   stores cannot modify main RAM; isolated cache reads, cache swapping and user
   mode remain unsupported. Address faults include the
   effective guest address. Unsupported instructions, devices, arithmetic overflow and
   branches in delay slots stop explicitly; synchronous exception entry is not
   implemented. IOP hardware interrupts can enter the guest RAM vector, recording
   Cause/EPC and branch-delay state, pushing status mode bits, and completing a
   preceding delayed load. Cause/EPC reads retain the coprocessor read delay;
   RFE restores the status stack, including in a return jump's delay slot.
   Firmware-vector mode stops explicitly without mapping a BIOS. User-mode
   returns, software interrupts and precise interrupt timing remain unsupported.
   RAM access and device access are separate. An optional IOP device interface
   receives aligned physical addresses after alias translation; unsupported
   accesses fault without retiring the instruction. The reboot context owns a
   persistent bootstrap-device instance. It currently retains word accesses to
   ICFG at 0x1f801450 in PS2 mode and DMA control at 0x1f801578. PS1 mode and
   other registers remain unsupported. DMA control stores enable/disable state;
   this does not implement IOP DMA channel execution or completion interrupts.
   The IOP interrupt controller separately retains pending causes, source masks
   and the master gate. Aligned word accesses support zero-bit acknowledgement,
   mask updates, master enable/disable (zero/one), and read-to-disable master
   control. Rejected widths or control values have no side effects. Raising a
   source latches it even while masked; CPU delivery additionally requires IEc
   and the IP2 mask. A partial native intrman provider exposes an export table
   in caller-allocated RAM. Its runtime-owned trampolines use an explicit host
   call interface; arbitrary guest syscalls are never treated as service calls.
   Supported operations register/release primary and DMA handlers, change primary masks,
   suspend/resume master control, and query interrupt context. Guest pointers are
   validated before state changes. Unknown ordinals stop explicitly. DMA enables/disables update the same
   DICR/DICR2 bank used by MMIO, preserving pending completion flags and returning
   the prior per-source mode on disable. When the disc archive imports intrman without supplying it,
   reboot allocates the native provider through the guest memory allocator. An
   original position-independent registration IRX is inserted after the memory
   and module managers in the boot list. It imports loadcore ordinal 6 using the
   supplied manager's export ABI, registers the native table through guest code,
   and writes a completion marker only after a zero return. A rejected call stops
   explicitly. The registration IRX and its handoff buffers remain allocated
   during startup. Supplied intrman providers are not replaced. Primary interrupt handlers now
   dispatch through the native provider, including DMA channel cascades.
   Thread context/preemption callback registration and reset retain validated
   guest entries. Voluntary thread switching now dispatches the context callback;
   primary interrupt-driven preemption uses the registered guest callbacks.
   The same registration path supplies a partial dmacman provider when absent.
   Both services reuse one native export/trampoline builder and call router.
   Reboot tracks every allocation it makes and rejects overlap before installing
   another provider or handoff buffer. The DMA controller shares channel address,
   block and inactive-control registers between MMIO and service calls for
   channels 0–12, with tag registers only on supported channels. Primary and
   secondary priority registers support channel priority and enable updates.
   Transfer starts, unimplemented dmacman exports and unsupported DMA operations stop
   without latching a transfer or manufacturing completion. Native registration
   success therefore does not establish functioning DMA transfers.
   A native heaplib provider uses the same registration path. It owns heap
   metadata in C++, reserves backing blocks through the initialized guest sysmem
   allocator, and supports aligned allocation, coalescing, optional growth,
   release of empty growth blocks, deletion and free-size queries. Heap handles
   and returned pointers refer to actual reserved IOP RAM. Backing allocations
   are checked against guest block extents and other owned regions. Nested
   sysmem calls use a separately reserved stack. Private chunk-helper ordinals,
   guest inspection of heap internals and interrupt-context heap calls remain
   unsupported; no proprietary heap metadata layout is reproduced.
   CP0 processor revision reads return the IOP R3000A revision with a load delay;
   writes remain unsupported.
   Reboot devices borrow an explicit SBUS interface from the SIF transport.
   Aligned IOP word accesses share command addresses, MSFLAG/SMFLAG ownership,
   and control bits with the EE side. IOP control toggles preserve EE-owned state;
   EE control writes preserve IOP state. The supported F260 view reads zero after
   writes, following the reference hardware model. Unknown offsets, partial
   accesses and unsupported control bits fault without side effects. An isolated
   reboot context with no SBUS attachment does not fabricate register values.
   This register connection does not move reboot RAM into the active HLE service
   transport or provide IOP DMA channel execution.
   The unmapped kernel cache-control address 0xfffe0130 retains diagnostic
   configuration and is not aliased into the physical-device range. The backend
   fetches coherent instructions and models no cache timing, stale lines or tags;
   this is a scoped flush-path approximation, not accurate cache emulation.
   This configuration register is not the SIF command/reply FIFO.
   A resumable startup call binds a loaded module, validates its RAM/stack ranges,
   installs its image, packs bounded `argc`/`argv` strings and pointer tables, sets
   GP/SP/RA, and executes until the entry returns, faults or exhausts its budget.
   A typed register-argument entry form supplies A0 through A3 directly for
   interfaces that do not take argc/argv, reusing the same validation and call
   lifecycle. It does not infer an entry ABI from a module filename.
   Resident function calls reuse owner bounds and GP while executing the current
   RAM image, without rebinding imports or reinstalling initialized data/BSS.
   They validate the selected entry before touching the argument save area.
   Return detection occurs after the return delay slot. It reports the actual
   return value without interpreting it as successful service initialization.
   RAM, stack and provider lifetimes belong to the caller; full allocation,
   scheduling and startup services remain unfinished. This is a partial reference
   interpreter, not an IOP native-code generator. SIF reset uses it for the
   memory bootstrap and manager entry and stops before claiming reboot completion.
2. The decoder and analysis worklist discover direct control flow, including
   delay slots. Unproven indirect destinations stop. Observed roots and
   image-matched RAM captures provide evidence for copied code.
3. The generator emits explicit guest-width C++ operations. Bounded non-inlined
   dispatch functions control function size and stack use. A shared loop handles
   PCs, delay state, budgets, code guards, and runtime stops.
4. The runtime owns register state and RAM. The system bus adds partial timer,
   INTC, DMAC, and partial SIF DMA transport. A separate BIOS-free IOP command
   endpoint owns IOP RAM and protocol state. Unsupported MMIO faults. Timer counting
   uses instruction-based diagnostic time, not accurate PS2 cycles.
5. The BIOS-free kernel implements startup services and a guest-visible syscall
   table. Guest handlers execute generated code through bounded nested
   continuations. The guest table is authoritative. INTC and DMAC delivery share
   a context-preserving callback dispatcher. A separate thread module manages dormant/ready
   records and priority-based context switching after supported kernel services.
   It saves integer and FPU state, preserves the CPU's global budget and CP0
   control state, and resumes the correct syscall/delay-slot continuation.
   Equal-priority readiness uses insertion order; a running thread is not
   preempted by an equal-priority start. Guest kernel continuations cannot switch
   threads. FIFO semaphore waits can block when another thread is runnable;
   signals wake the oldest waiter. SleepThread blocks until a wakeup or consumes
   a previously queued wakeup. Wakeup and cancellation expose the queued count
   through thread status. With no runnable thread, the runner spends budget on
   device steps without incrementing retired guest instructions. Interrupt handlers
   may wake sleepers; scheduling waits until callback context restoration.
   Timeslice preemption remains unsupported. See [SIF transport](sif.md) for
   device and protocol boundaries.
6. Small programs parse arguments and invoke libraries. The build tool passes
   explicit arguments to processes without constructing shell commands.
7. The FILEIO service owns read-only file handles and uses bounded ISO range
   reads. SIF packet parsing and DMA reply transport stay in the hardware layer.
   Guest paths resolve only within the explicitly mounted image. See
   [file-I/O service](fileio.md) for protocol scope and limits.

GT4 is the first integration target. Game identifiers and observed addresses must
not select special behavior in generic libraries. Additional code entry points
are analysis inputs, not game-specific runtime patches.

## Copied and modified code

A capture contains RAM and an image fingerprint. The fingerprint detects
accidental mismatches; it is not cryptographic authentication. Capture discovery
starts at supplied observed entries. Conflicting instruction versions at one
address are rejected. Generated programs start from the original ELF and replay
its copies, rather than restoring a captured machine state. Instruction guards
prevent silently executing stale translations.

Commercial captures, generated C++, and executables are local-only artifacts.
Synthetic tests exercise the same mechanisms without commercial data.

## Performance direction

Current per-instruction dispatch and guards favor diagnosability. Future work
should form basic blocks, use direct known flow, share slow paths, hoist guards
only with valid invalidation, and batch execution to event deadlines. Keep
functions bounded and avoid duplicated runtime logic. Benchmark Release builds
with reproducible workloads after correctness tests. AOT alone does not establish
performance.

## Acceptance boundaries

GIF has a separate initialization register model. CTRL reset clears GIF mode and
pause state without resetting GS display registers or DMA configuration. MODE
path-3 masking and intermittent mode appear in STAT. The receiver exposes its
last tag and active path; detailed hardware FIFO counters are not modeled.
Unsupported CPU FIFO accesses, register widths,
holes and status/tag writes fault explicitly.

The three graphics DMA channels share idle configuration handling for CHCR,
MADR, QWC, TADR, ASR0/1 and zero initialization of the offset-0x80 slot. QWC is
16-bit. Reverse/interleave modes and nonzero scratchpad-register
setup fault before mutation. VIF0/VIF1/GIF transport reads RAM or scratchpad, consumes
one tag or quadword per diagnostic step, and honors DMAC enable/suspend gates.
Normal mode and REFE/CNT/NEXT/REF/END chains support tag IRQ termination.
TTE is supported on VIF channels; GIF TTE remains unsupported.
Completion clears STR and raises the channel's D_STAT bit only after consumption.
CALL/RET/REFS, priority arbitration and active channel rewrites remain unsupported.
D_PCR stores CPC/CDE configuration only with priority control disabled; enabling
PCE is unsupported. No initialization operation fabricates DMA completion.
GIF paths 2/3 share a serialized decoder for PACKED A_D/PRIM/NOP and REGLIST packets, honoring NREG zero as
sixteen, ignores odd REGLIST padding and applies PRE/PRIM. Each beat stages parser
and GS configuration state before committing. Other packed forms and VIF/VU GIF
paths remain unsupported. GS configuration is stored separately
from privileged display registers. SIGNAL/LABEL apply masked ID updates; FINISH
and SIGNAL latch CSR events and use existing GS interrupt delivery. Accepted work
is synchronous. Drawing, local-to-host/local-to-local transfers and queued SIGNAL stalls stop
explicitly, so accepted configuration does not imply rendered pixels.

IMAGE/IMAGE2 packets deliver host uploads into four MiB of GS local memory.
PSMCT32/PSMCT24 use a shared page/block/column address permutation, with 24-bit
uploads preserving each destination's high byte. Partial pixels survive packet
boundaries; padding after the final pixel in a beat is ignored. Transfer setup
latches destination parameters and validates format/traversal before mutation.
GS reset cancels transfer state without clearing local memory. Configuration
transactions share storage without copying four MiB per command; only the
validated upload path mutates pixels. Other formats and reverse traversal stop.
LWC1/SWC1 move raw 32-bit FPR values through the bus, preserving bit patterns and
using ordinary memory alignment checks. These operations perform no host floating
point arithmetic.

VIF0/VIF1 share an initialization register model with independent state. Cold
state is idle with an empty FIFO; soft reset clears status and error masks while
preserving configuration and row/column data. FBRST stop, force-break and cancel
operate on idle units, and command bits are not retained. Widths, alignment,
register holes, reverse transfers and unsupported command-managed writes are
checked before mutation. Direct CPU FIFO access remains unsupported.
The shared DMA receiver accepts NOP, STCYCL, STMOD, MARK, STMASK, STROW/STCOL,
idle FLUSHE/FLUSH and MPG. BASE/OFFSET configure VIF1 buffer addresses, with
OFFSET clearing DBF and latching BASE into TOPS. ITOP stages ITOPS on either unit
without changing the launch-time ITOP register. FLUSH is supported only while
VU execution and GIF path 1 cannot become active; it waits for pending path-2
payloads. FLUSHA remains unsupported.
VIF1 DIRECT assembles four-word GIF quadwords across DMA beats and submits them
to path 2. Zero immediate means 65,536 quadwords. Pause or another path's open
packet retains the DMA beat for retry; packet ownership lasts through EOP.
Path-3 masking does not mask path 2. This is serialized packet arbitration, not
cycle-accurate GIF scheduling. DIRECTHL and FLUSH sharing the beat that completes
a DIRECT payload remain explicit unsupported cases.
Pending payloads survive DMA beat boundaries. MPG stores
words in separate per-unit micro-memory with address wrapping; execution is still
rejected. Each beat stages register changes and at most four memory writes before
committing, without copying micro-memory. Unsupported commands leave the current
beat unchanged. Soft reset discards a pending command and retains micro-memory.
DMA scheduling and command state are functional approximations, not bus timing.

CFC2/CTC2 generation forwards VU control accesses to a separate device interface.
FBRST supports the two units' control masks and reset strobes; VPU_STAT reports
idle because every execution-start path is still unsupported. Interlocked forms
are valid only under this idle-unit limitation. Unsupported control registers and VU
microprogram execution stop explicitly. These interfaces require no BIOS code.

VI0 is constant zero; VI1 through VI15 store 16-bit values and read back with
zero extension. MAC is read-only. Scalar MOV.S preserves raw bits, while ABS.S
and NEG.S change the sign and clear current overflow/underflow flags. Scalar
comparisons use integer ordering of normalized bit patterns, treating denormals
as zero and exponent-255 inputs as signed maximum finite values. BC1 normal and
likely branches reuse the existing branch and delay-slot machinery.

MUL.S computes finite products using an exact 48-bit integer significand product
and explicit chop rounding. Denormal inputs become signed zero; underflow flushes
to signed zero and sets current/sticky underflow flags. Successful operations
clear current overflow/underflow flags before applying the new result, retaining
unrelated and sticky flags. Extended exponent operands/results stop before any
destination or flag mutation. PS2 partial-product precision quirks are not yet
modeled, so this is not a bit-exact hardware multiplier implementation.

CVT.S.W normalizes the signed integer magnitude and truncates excess fraction
bits without host floating-point operations or flag changes. DIV.S uses integer
quotient/remainder rounding to nearest-even, with signed-zero/denormal handling,
saturated zero-denominator results and current/sticky I/D flags. Underflow flushes
to signed zero without changing U/O flags. Extended exponent operands/results
stop before destination or flag changes; full EE numeric behavior is unfinished.
Scalar ADD.S/SUB.S share the finite significand core with vector addition and
subtraction. Scalar operations apply FCR31 U/O flags, while VU operations retain
their separate per-lane MAC and STATUS updates. The shared result carries an
underflow indication, keeping numeric arithmetic separate from device flags.
CVT.W.S converts bit patterns to signed words with truncation toward zero and
sign-dependent saturation outside the representable range, preserving FCR31.
MIN.S/MAX.S select raw operands using integer ordering, including signed zero,
denormals and exponent-255 patterns, and clear only current U/O flags.
Packed PEXTLB/H/W and PEXTUB/H/W share a compile-time lane-width/half selection
helper. Both operands are captured before alternating Rt and Rs lanes into the
128-bit destination, preserving correctness when a source aliases the result.

The VU0 register model also supports serialized macro VADD/VSUB, masked VMOVE/VMR32,
QMFC2/QMTC2, and LQC2/SQC2. VF0 is always (0,0,0,1); writes are discarded but
arithmetic targeting it still updates flags. Addition and subtraction share a
single implementation for lane-wise operands and x/y/z/w broadcasts, snapshotting both operands before
updating selected lanes, clears inactive MAC lane flags, and accumulates sticky
STATUS flags. All lane results and flags commit together after validation.

Finite addition/subtraction uses integer significands with sticky alignment bits and rounding
toward zero. Denormal operands become signed zero; underflow results flush to
signed zero and set underflow/zero flags. Exponent-255 operands and results that
require the extended exponent range stop explicitly. This is a bounded numeric
implementation, not complete VU arithmetic. Macro results/flags are immediately
visible at the next instruction; pipeline latency and concurrent microprogram
execution are not modeled. VU reset strobes preserve vector data, consistent with
the existing control reset scope.

Vertex assembly lives in `gs_vertex.cpp`: XYZ2/XYZF2 and XYZ3/XYZF3 capture
position, depth, fog, RGBAQ, ST and UV attributes. Lists consume their complete
vertex set, strips retain their trailing vertices, and fans retain their anchor.
XYZ3/XYZF3 suppress the draw while still advancing assembly. PRIM writes restart
the pending primitive. `gs_sprite.cpp` validates draw state and submits solid or textured
sprites to the separate `nt_vulkan` library. Its GLSL compute shader implements
swizzled local-memory addressing. Both drawing contexts, PRMODECONT,
XYOFFSET, scissor, framebuffer masks, FBA and PSMCT32/24 are supported. Alpha
tests, destination-alpha tests, fog/blending, depth writes and nontrivial
depth comparisons stop before pixel changes. ZTE disabled also disables depth
writes. Other primitive types remain unsupported at drawing time.
GS transactions share local memory until a pixel writer detaches its storage;
this prevents a later failed command in the same GIF beat from leaking pixels.
This initial copy-on-write approach favors transaction correctness and has not
been performance-optimized for large draw workloads.

The Vulkan backend owns its instance, selected compute device, mapped GS buffer,
pipeline, descriptors, command buffer and fence. Build-time `glslc` embeds SPIR-V
from the original shader source. Each synchronous draw uploads and reads back the
four-MiB GS buffer; this establishes a real GPU path but is not the final streaming
architecture. Atomic masked pixel updates handle framebuffer address aliasing.
Nearest fixed-UV textures support PSMCT32/24, MODULATE/DECAL, TCC, TEXA/AEM and
all four CLAMP modes. Integer UV interpolation uses floor division for negative
gradients. Texture reads use a separate four-MiB snapshot of local memory for
the draw, avoiding read/write races with the framebuffer; this is a bounded
texture-cache model, not complete GS texture-cache emulation. Textured draws
with aliased framebuffer destinations are rejected. Mipmaps, mixed min/mag filters,
perspective coordinates and HIGHLIGHT modes remain unsupported.
PSMT4 indexed sampling uses an uploaded sixteen-entry palette. The shared
GS CLUT stores 256 32-bit entries; TEX0 loads honour CSA banks, CSM1/CSM2,
CLD controls and both remembered CBP values. CLUT mutation remains part of the
small GS transaction. 16-bit palette formats and other indexed formats remain
unsupported. PSMT4 texture addressing is computed in the Vulkan shader.
Matching TEX1 minification/magnification modes select nearest or bilinear
sampling. Bilinear sampling subtracts half a texel, resolves and expands each
of four samples independently, then interpolates channels horizontally and
vertically using four-bit weights before applying the texture function.
The native Windows host additionally creates a Vulkan surface and FIFO swapchain
and presents a synthetic scene produced by the same compute shader. Presentation
currently requires a fixed matching image size; resize/recovery and game scanout
integration remain unfinished. No SDL dependency or CPU fallback is present.

The hardware library owns a separate GS privileged-register model. Display
configuration is stored without producing pixels. CSR exposes
an empty FIFO and reference-compatible identity, accepts acknowledgements and
reset, and does not latch write-only commands. IMR preserves its fixed bits.
GsGetIMR/GsPutIMR forward through the bus, including interrupt-safe aliases.
Unknown registers, quadword accesses, local-to-host and external feedback
transfers stop explicitly.

SetGsCrt configures interlaced NTSC (mode 2) or PAL (mode 3), with either
field/frame framebuffer mode reflected in SMODE2. Unsupported modes fail before
changing state. This is host configuration, not a reconstructed BIOS register table.
An integer half-line clock uses a 294.912 MHz EE time base, 60000/1001 NTSC
fields per second or 50 PAL fields per second, with fractional cycles retained.
NTSC/PAL fields contain 525/625 half-lines. VBlank starts at half-line 480/576,
GS VSINT and FIELD update at 487/582, and VBlank ends at the field boundary.
INTC lines 2/3 receive blanking edges independently of GS IMR; GS events latch
CSR and raise INTC line 0 when unmasked. Unmasking a pending event also raises
the line; acknowledging INTC alone does not repeatedly retrigger a latched GS event.
Horizontal sync is approximated at whole-line boundaries, not pixel-clock phase.

Timing advances only after SetGsCrt and stops on CSR RESET. The runtime charges
one diagnostic cycle per retired instruction, so these nominal video intervals
are not cycle-accurate PS2 timing or real-time pacing. Direct timing reprogramming,
progressive modes, gated/HBlank timers, general GIF rendering and game scanout remain
unsupported. Active unsupported timing changes fault instead of silently retaining
a mismatched clock configuration.

- Compiler correctness: generated synthetic programs pass semantic tests.
- Startup progress: an opt-in integration test reaches a documented boundary.
- Boot: verified display and input through the game's startup sequence.
- Playability: sustained rendering, input, sound, loading, and gameplay.

Only the first two have implementation evidence so far. An expected diagnostic
stop is a passing regression test, not a successful game boot.

IOP timers currently provide the six count/mode/target register banks, 16/32-bit
widths, internal-clock prescalers, counter stop modes, and polling flags. The
diagnostic CPU supplies one timer tick per retired instruction; this is not
cycle-accurate timing. Mode reads clear target/overflow flags. Target-reset counting and target/overflow interrupts feed the shared IOP interrupt
controller. Pulse/toggle and one-shot/repeated modes retain their rearm state;
mode reads clear event flags and rearm requests. External clocks and blanking
gates remain unsupported and reject writes before changing timer state.

IOP DMA interrupt control exposes both DICR banks, channel enables, force IRQ,
computed status and write-one-to-clear completion flags. A completion signal
interface routes sources into primary IRQ 3; it does not perform transfers.
SIF0/1 completion sources retain their unmaskable tag-interrupt behavior. The
SIF1 destination-chain and SIF0 source-chain START are supported with guest RAM.
Additional DMA transfer paths remain unfinished; native DMA interrupt dispatch is supported.

Disc-backed IOP reset runs in bounded 64-instruction slices during SIF device
advancement, after the reset packet finishes transport. This is diagnostic
scheduling, not a PS2 clock ratio. EE execution and SBUS writes continue between
slices; EE-owned flags are preserved while IOP readiness and old HLE service
state are cleared. The EE SIF initialization service publishes its transport
flag only after configuring its receive channel. Faults remain stopped with
the original diagnostic. Guest SIF1 receive is implemented; other IOP DMA transfers and full service
handoff remain unfinished. DMA channel registers support halfword updates preserving the other
half, with the same START validation as word stores.

Guest SIF1 receive validates the full destination extent before consuming a
tag, copies bounded FIFO payload slices into reboot RAM, advances MADR, preserves
partial packets across channel pauses, and clears START on terminating tags.
Completion flags route through DICR2 and primary IRQ 3. The old HLE packet parser
is bypassed while disc modules run. Submitted EE jobs complete as soon as their
last packet is received. Guest allocator invocations share the installed native
service router, so their intrman calls follow the same path as module startup.

The IOP CD/DVD initialization controller owns byte-wide command/status ports,
bounded normal/subcommand parameter FIFOs and a draining result FIFO. NOP
completion raises IRQ 2 through the shared interrupt controller; interrupt
status is acknowledged by writing the selected bits. Parameter errors retain
command error status and clear the error byte on read. Tray-state queries and
sticky-status acknowledgement operate on the stopped controller state. Disc
type, sector transfers, firmware contents, NVM and other commands remain explicit
unsupported accesses; this layer does not fabricate drive firmware or media.

CdReadRTC latches one host-clock timestamp per command and returns the device
BCD layout in UTC+9. Software remains responsible for local time-zone adjustment.
The clock source is injectable for deterministic tests; draining a response
never samples a second timestamp. RTC writes remain unsupported.

IOP LWL/LWR/SWL/SWR operate on aligned RAM words with little-endian byte
selection. EE and IOP reuse one unsigned word-merge helper; EE-specific widening
remains in the EE runtime. Partial IOP loads merge from a pending load to the
same destination, preserving ordinary load-delay visibility across budgets.
Partial MMIO accesses stop before device reads or writes; cache-isolated stores
do not modify main RAM and isolated reads remain unsupported.

The mechanical-controller version query returns the documented public fallback
compatibility profile 03 06 02 00 through the normal result FIFO. This identifies
the register ABI being modeled; it does not enable missing media, security or
firmware-dependent commands. The profile is independent of disc identity.

CancelPOffRdy clears the emulated controller power-off-ready bit and returns its
one-byte command result. It does not perform host power-management operations.

The IOP device map exposes an original read-only ROMDIR-format resource directory
at physical 0x1fc00000. Its default contents are RESET, ROMDIR and EXTINFO metadata
and a terminator; RESET is empty and no firmware files are supplied. Explicit
resources use bounded, aligned payloads and the shared archive validator. Only
the populated extent is readable. Instruction fetching remains RAM-only, and
resource writes and unmapped reads fail. This supports directory discovery, not
BIOS execution or unavailable firmware services.

The native intrman provider handles guest thread-yield syscall 0x20 using the
public 0x98-byte context ABI. It saves the voluntary frame on the outgoing stack,
executes the registered guest scheduler callback through the normal budgeted CPU,
and restores either a voluntary frame or a full newly created thread frame.
It validates frame extents, control state and executable RAM targets before
restoration. An active return gate is owned by intrman; other native providers'
unimplemented entries cannot fall through into this guest syscall ABI. Other
context formats, nested yields, delay-slot yields and firmware vectors remain
unsupported. Primary hardware interrupts dispatch through native intrman; DMA
cascade dispatch uses per-channel guest handlers.

Native IOP primary interrupt delivery selects the lowest pending enabled source,
validates its handler, masks and acknowledges it, then executes guest code on a
dedicated 2 KiB stack reserved with the provider allocation. A nonzero handler
result reenables the source; zero leaves it masked. Interrupted registers, HI/LO,
load completion and branch-delay continuation survive budgeted handler execution.
The registered preemption callback can request a full-context save and guest
scheduler selection using the existing thread-switch bridge. Native return gates
are accepted only during their corresponding active callback. Missing handlers,
forced DMA interrupts and nested native interrupts stop explicitly, preserving pending
state, instead of entering an uninstalled RAM exception handler. Raw CPU vector
execution remains available when no native provider handles interrupts.

CpuInvokeInKmode (intrman export 14 and guest syscall 0x0c) invokes guest RAM
code through the existing CPU execution path. It forwards the three register
arguments, retires the prior pending load, reserves callback stack space, and
enters kernel mode with CPU interrupts disabled. A bounded stack of at most
32 wrapper records restores the continuation, stack, return address and status
while retaining guest results and memory changes. Nested calls and execution
budget suspension are supported. Thread switching while a kernel callback is
active remains unsupported; invalid entry points or stack extents stop before
entry. The return gate is accepted only with an active wrapper record.

The native DMA cascade drains pending channels from both controller banks in
ascending channel order. Each W1C acknowledgement preserves other channel flags
and control bits. Guest handlers run on the existing budgeted interrupt path;
a zero return clears that channel's completion enable. The primary cascade is
reenabled after all channel causes drain, before normal preemption processing.
Unregistered channel handlers and forced DMA interrupts remain explicit stops.
SIF0 source-chain output feeds the same FIFO consumed by the EE receiver.

IOP SIF0 source chains transfer at most one quadword per device step and honor
priority gating, FIFO capacity and pause/resume. Tag and payload extents are
validated before tag emission. Each tag advances TADR by 16 bytes; the EE tag
occupies a full FIFO quadword, including the following descriptor's ignored
words. Partial payload quadwords retain trailing lanes from the preceding
quadword. Stop/IRQ tags clear START and raise channel 9 completion after payload
transfer. Normal-block modes remain unsupported; timing is diagnostic.

IOP analysis retains a call's return continuation even when its indirect target
set is unknown. The delay slot is still decoded and validated; reachable unknown
transfers are reported together with their source register. Unknown indirect
jumps have no assumed fallthrough. This does not authorize an arbitrary runtime
call: all unresolved transfers still reject conversion before source or LLVM
emission. Mutable callback tables need target recovery across registration and
use; their initial contents are not proof of their later values.

### Native RPC initialization

InitRpc (sifcmd 14) uses the existing command session and shared system-status
event. It first waits for command setup (bit 0x100), then publishes a SetSreg
packet for RPCINIT index 0 with value 1 exactly once per session. A busy peer
keeps the import pending; a failed send does not mark readiness as published.
It finally waits for the explicit peer RPC acknowledgement (bit 0x800).
The mode argument is unused, matching the public contract.

The first wait retains the native import PC through a retry-yield service reply.
The scheduler suspends the caller on the event and retries that import when it
wakes; it does not retire the import or return to the caller between phases.
The final wait uses the ordinary event continuation. Repeated initialization
shares publication state and does not reset registered RPC servers or queues.
A standalone caller without a scheduler can retry the final acknowledgement wait
explicitly. Missing peers, stopped links and interrupt-context initialization
remain unsupported. No peer acknowledgement or client operation is fabricated.

### Native RPC worker loop

RpcLoop (sifcmd 22) reuses native queue ownership, compiled handler invocation,
reply delivery and thread sleep/wakeup. It requires the queue's registered worker
to be the current native thread. A queued request is validated and its saved
continuation allocated before removal from the FIFO. Reply completion restores
the loop import PC and queue argument, rather than returning to the original
caller. Both reply modes and retry snapshots use the same ExecRequest path.

An empty queue clears its public active/start fields and sleeps through the
thread provider. Wakeup resumes at the loop import. An existing wakeup credit
consumes one budgeted service operation before rechecking, preventing an internal
unbounded busy loop. No generated instructions are decoded and no guest kernel
loop is executed. Detached queues, absent schedulers and mismatched worker IDs
stop explicitly. Concurrent peer ownership still requires future synchronization.

### Native thread priority changes

ChangeThreadPriority (thbase 14) updates the existing cooperative scheduler.
Thread ID zero selects the current continuation; priority zero inherits the
current continuation's priority. Explicit priorities range from 1 to 126.
Unknown handles, dormant workers, invalid priorities and interrupt-context calls
return the public errors without changing scheduling state. Resolving a current
thread without an active scheduler remains unsupported.

A changed ready worker moves to the back of the ready list, whose stable minimum
selection preserves FIFO ordering within each priority. Lowering the current
worker to an eligible ready priority, or raising another ready worker above it,
yields at the compiled service boundary. The normal scheduler saves and resumes
the caller. Sleeping workers keep their wait state with the updated priority.
This supplies cooperative scheduling behavior, not asynchronous preemption or
interrupt-context iChangeThreadPriority.

### Native thread status

ReferThreadStatus (thbase 22) writes the public 68-byte foreground status record
from native scheduler state. It distinguishes the current running thread, ready
workers, sleeping/event-waiting workers and dormant entries. Thread ID zero
selects the current continuation. Creation attributes, option, entry, allocated
stack, GP, initial/current priority and queued wakeups are retained explicitly.
Restarting a created worker restores its initial priority.

Event suspension now records the native event handle; completion clears it.
Sleep reports its wait type with no event handle. The entire output region is
validated for bounds, alignment and generated-code protection before writing.
Unknown thread handles and interrupt-context queries return public errors.
The foreground record's interrupt-only register-context field and reserved words
are zero; no host pointer is exposed. iReferThreadStatus remains unsupported.
Host-admitted continuations expose their known entry and GP, with zero stack
allocation metadata because the scheduler does not own their stack.

### Native delayed workers

DelayThread (thbase 33) suspends the current native continuation until a steady
host-clock deadline. It records the original microsecond argument in thread
status and applies a 100-microsecond minimum wait. Deadline arithmetic is checked
before changing state. Tests may inject a clock; production uses steady_clock.

The scheduler releases expired delays at its normal execution boundaries when
interrupt delivery is enabled. Zero-budget calls cannot release them. Ordinary
WakeupThread does not shorten a delay; its wakeup credit remains for a later sleep.
Completion clears delay wait metadata and resumes the already-retired service.
This is cooperative host scheduling, not cycle-exact console timing or an
asynchronous preemption mechanism. A worker that does not yield can defer another
worker's resumption until the next scheduler boundary.

The native probe chooses the earlier of its alarm and thread-delay deadlines for
host sleeping, retaining its existing bounded startup wait. No guest timer
registers, instruction-count delays or busy-loop instructions are required.

### Native semaphores

The thsemap native provider supports ordinals 4-9 and 11-12: creation, deletion,
foreground/interrupt signalling, waiting, polling and foreground/interrupt status.
It owns bounded semaphore records and monotonic handles. Counts are bounded by a
positive signed maximum; unsupported count configurations and priority-inheritance
attribute 0x100 stop explicitly. Unknown attribute bits return the public error.

An empty WaitSema suspends its compiled continuation through NativeThreadWaits,
shared with event flags. Thread status reports semaphore wait type and handle.
Signal transfers one permit directly to one waiter; it does not also increment
the count. Attribute zero preserves FIFO arrival order; attribute one selects the
lowest current native thread priority with FIFO ties. Priority changes while
waiting therefore affect subsequent selection. Foreground wake/delete operations
can yield through the scheduler; interrupt signalling never switches inside the
callback. Deletion resumes pending waiters with the public deleted-object error.
Polling and count-overflow errors do not change state. Status preserves reserved
words and validates the full output record before writing.

These are native cooperative synchronization objects, not host-blocking semaphore
calls or an emulated thread manager. The standalone converted host connects the
provider explicitly. Priority inheritance and arbitrary thread termination while
queued are outside the currently supported lifecycle.

### Native message boxes

The thmsgbx provider supports ordinals 4-9 and 11-12 through bounded native queue
ownership. Public packet addresses retain their aliases, and the packet's circular
next links are mirrored in RAM. The host never follows those links to find work.
FIFO messages preserve arrival order; priority messages are inserted by their
unsigned priority byte with stable ties. Duplicate or overlapping queued message
headers, including aliases across boxes, stop before mutation. Queued headers and
receive/status outputs are validated for bounds and generated-code protection.

ReceiveMbx uses the public output-pointer-first argument order. An empty receive
suspends through the shared native wait interface with mailbox wait type/handle.
SendMbx delivers directly to one waiting receiver when present; otherwise it
queues the packet. Receiver selection uses FIFO or current native thread priority.
Interrupt sends wake without switching inside the callback. DeleteMbx releases
waiting workers with the public deleted-object error and releases queue ownership
without freeing caller-owned messages. Polling an empty box returns its public
error. Status exposes counts and the next packet while preserving reserved words.

Queues are limited to 4096 messages per mailbox and handles are never recycled.
Concurrent peer/thread mutation remains outside the serialized native scheduler
contract. Arbitrary termination of queued waiters is not implemented.

### Native kernel diagnostics

Sysmem Kprintf (ordinal 14) shares the existing native stdio formatter and explicit
host output sink. Its O32 register/stack arguments, bounded strings, formatting
limits, return count and sink failures use the same implementation as printf.
Heap services continue to own only allocation/query ordinals; the router passes
Kprintf to the diagnostic provider. No output provider means an unsupported call,
not a fabricated successful print.

A null Kprintf format is callback-specific control rather than text and remains
unsupported. Guest KprintfSet callback registration is also unsupported. The
native host sink is the explicit replacement for kernel diagnostic output.

### Native RPC peer reads

GetOtherData (sifcmd 23) uses a separate native data provider backed by an explicit
converted-peer endpoint. Since the native owner exposes a serialized memory view,
the provider replaces request/response transport with a bounded, snapshotted copy.
Both blocking and NOWAIT mode complete before return. It does not consume or
publish command packets, and an unrelated occupied command mailbox does not block
this native operation. A missing or stopped peer remains unsupported.

The O32 fifth argument supplies mode. Source/destination aliases are normalized;
byte-aligned data up to one MiB is supported. Descriptor bounds, output code
protection, mode, source extent and descriptor/output overlap are checked before
writes. Zero-length reads do not dereference unused data pointers. Successful
completion clears the descriptor packet pointer, assigns a native request ID and
records that no semaphore is needed. Remaining descriptor fields are preserved.
Failures leave data and descriptor untouched. Null or uninitialized packet fields
in caller-owned receive descriptors are not followed as host pointers.

This is an intentional native transport replacement, not emulation of remote
packet latency. It requires serialized access to the supplied peer memory;
concurrent ownership would need synchronization. It does not implement client
BindRpc/CallRpc or inbound remote-data packet handling. The standalone probe has
no converted EE endpoint yet and therefore cannot fabricate successful reads.

### Native disc metadata

Ioman getstat (ordinal 16) uses the shared disc filesystem to resolve file or
root-directory metadata without opening a handle or advancing a cursor. ISO
entries retain their seven-byte recording-date field. The native provider writes
the 40-byte legacy io_stat layout with read-only regular/directory mode bits,
zero attributes/high-size and the actual extent size. Creation/access/modification
fields use the disc recording date, as the public disc-driver contract does.
The stored wall-clock fields are preserved without host timezone conversion.

Paths remain confined to cdrom/cdrom0. The root accepts slash/backslash forms;
traversal and absent paths retain normal filesystem errors. The path is captured
before output writes, and the entire output is checked for alignment, bounds and
compiled-code protection. Lookup errors leave the output untouched. Device registration and writable media remain unfinished.

### Native directory handles

Ioman dopen/dclose/dread (ordinals 13-15) enumerate the user-supplied ISO through
the shared disc filesystem. Directory and file handles share the 64-descriptor
limit and number space. Each opened directory owns an independent snapshot of
its entries and cursor. Files cannot be opened as directories; byte reads/seeks
reject directory handles, and file/directory close operations check handle type.

Dread validates the entire 300-byte legacy directory record before advancing the
cursor. It reuses getstat's metadata encoder, preserves ISO entry names including
version suffixes, null-terminates the name and emits no host private-data pointer.
It returns one for an entry and zero at EOF; EOF leaves output untouched. Invalid
output storage leaves the next entry available for retry. The parser excludes
ISO self/parent records, and empty directories return EOF immediately. No paths
resolve against the host filesystem and enumeration does not modify the image.

### Read-only disc mutation contract

Ioman write (ordinal 7) rejects read-only disc descriptors with EBADF before
reading the source buffer, including zero-length writes. Standard streams 0-2
remain unsupported by the disc provider so a console provider can own them.
Remove, mkdir, rmdir, chstat and format (10-12, 17-18) resolve the bounded path's
device and return EROFS for cdrom/cdrom0. Foreign devices return ENODEV.

These are explicit errors for a mounted read-only filesystem, not successful
mutation stubs. No source/stat payload is dereferenced after the read-only check,
no handle/cursor changes occur, and the ISO is never opened for writing. This
does not implement writable memory cards, host filesystem writes or arbitrary
device-control operations.

### Native 64-bit disc seeks

Ioman lseek64 (ordinal 30 in the extended legacy table) decodes its O32 offset
from a2/a3 and whence from stack offset 16. The native service bridge optionally
returns a high word in v1 alongside v0, preserving ordinary 32-bit service
behavior. Pending register loads retire before the returned pair takes effect.

File cursors are 64-bit. Both seek variants use one checked position calculation;
legacy lseek enforces its signed 32-bit return range before committing. lseek64
supports nonnegative positions through INT64_MAX, including beyond EOF, while
negative positions and arithmetic overflow return EINVAL without changing state.
Negative error results are sign-extended across both return registers. Reads past
EOF return zero without truncating a large cursor. This expands cursor arithmetic,
not ISO extent support: the existing ISO reader still supports single-extent
32-bit file sizes and rejects unsupported disc layouts explicitly.

### Native disc descriptor controls

Legacy ioman ioctl (9) and ioctl2 (34) bind to the native disc provider. Invalid
disc descriptors return EBADF before examining caller buffers. Standard streams
and directory controls remain unsupported by this provider. The mount currently
opens only ordinary read-only files, so ioctl2 returns EIO, including stream
pause/resume/status requests. No streaming state or success is fabricated.

Unknown ioctl commands return EIO. Recognized hardware controls (processor/port
queries, spin control, decoding, disc keys and disc type) remain explicit
unsupported import stops until a native contract exists. These paths neither
read payload pointers nor write outputs, change the file cursor, or touch the
image. This is partial device-control coverage, not a complete drive service.

## Native driver lifecycle

Legacy ioman AddDrv/DelDrv (20/21) use an original native registry with 16 slots.
The descriptor, bounded name and first two operation pointers are validated
before dispatch. Initializer and teardown targets must already be compiled.
Constant AddDrv descriptors and their operation tables in initialized module
images seed both private lifecycle roots, including across bundle modules.

AddDrv saves a caller continuation, invokes init(device), and publishes only on
nonnegative return. Negative initialization returns -1 without a registration.
DelDrv finds the first matching registered name, calls deinit with the original
device pointer and registration GP, then removes it; legacy teardown return values
are ignored. Duplicate names retain slot order. Concurrent/reentrant lifecycle
requests return -1. Missing devices and a full table return -1. Interrupt-context
calls, malformed names and changed registered names/teardown targets stop rather
than silently substituting behavior. Other mutable descriptor semantics are not
implemented yet. No device operations are executed through host function pointers.

Lifecycle and RPC continuations carry a host-only owner tag in Cpu. It is excluded
from the LLVM state ABI and restored with the caller. A provider consumes only its
own return, allowing distinct services to nest without stealing continuations.
The unavailable service result claims an operation while preserving its explicit
unsupported stop; unlike unsupported, routers must not try another provider.

The standalone host installs the registry and compiled file provider ahead of
the ISO provider. During lifecycle calls, file dispatch remains unsupported.
Registration alone is not proof of a functional driver or game startup.

## Compiled driver file dispatch

NativeDriverFiles implements legacy ioman open/read/write/seek/close using native ownership
and already-compiled operations. Initialized registration tables also seed open,
close, read, write and seek callback discovery. The descriptor's operation-table pointer is
checked against the registered snapshot, and each selected function must be a
compiled instruction entry. Unknown operations remain explicit stops.

A shared NativeFileDescriptors pool reserves 0..2 for console providers and owns
64 ordinary descriptors. Native ISO handles are translated through that pool;
compiled drivers receive distinct descriptors from the same pool. A provider
never handles another provider's descriptor. Registered device names take
precedence over native ISO paths; unmatched names may reach the native mount.

Open splits the device prefix and decimal unit from a bounded path. It allocates
the public 16-byte iop_file record through the shared native heap, initializes
mode/unit/device/private-data fields, and calls open(record,relative-path,mode).
A nonnegative callback result becomes the native descriptor; failure releases the
record, descriptor and retained registration. Read checks the access mode and
entire destination before invoking read(record,buffer,count); the callback owns
file contents and private state. Close always frees the descriptor and record
after the compiled callback, even when its return is negative, matching legacy
manager behavior. DelDrv stops while a file retains the registration.

Pending calls preserve their caller and service owner across LLVM budget slices.
This initial provider serializes file callbacks and stops on nested/concurrent
file operations. Device controls for compiled drivers remain unfinished; the native ISO implementation retains its
existing supported operations. No interpreter or host-pointer dispatch is used.

Compiled writes reuse the descriptor access check and bounded buffer validation,
but treat the payload as readable input: source bytes may overlap compiled-code
data addresses. Zero-length requests do not validate the unused buffer. Both read
and write enforce the existing 1 MiB service bound. The converted driver performs
the transfer and supplies its result; the manager invents neither contents nor a
successful byte count. A write callback's own stores retain LLVM memory guards.

32-bit seek accepts only origins 0..2 before dispatch. It forwards the original
32-bit signed-offset bit pattern and origin to the compiled operation, preserving
its result and private cursor state. Read/write/seek share a value-returning
continuation; only open and close change handle ownership. Native ISO writes
remain read-only errors and never modify the input disc image.

Legacy compiled-directory dispatch uses the same native descriptor/heap ownership
as regular files. Dopen initializes mode zero for the legacy manager ABI and
invokes dopen(record,relative-path). A separate native directory flag prevents
file read/write/seek/close from consuming a directory handle and prevents directory
calls on ordinary files. Dread validates aligned writable space for the complete
300-byte io_dirent record before calling the compiled function. The driver owns
entry contents and its cursor; the manager forwards positive, zero/EOF and negative
results without inventing metadata. Dclose frees the record and descriptor after
the callback; failed opens use the existing rollback path.

The initialized operation table supplies dopen/dclose/dread discovery roots.
Legacy devices receive the 300-byte caller record directly; extended devices
receive a separate aligned record as described below. Device type and operation-
table pointer changes after registration stop explicitly before file dispatch.

Compiled-driver lseek64 dispatch requires the extended device type and operation
slot 22. Discovery reads that slot only from descriptors with known extended type
bits, avoiding overreads of ordinary legacy operation tables. O32 arguments retain
the even a2/a3 offset pair and validated origin at sp+16; only a0 changes from the
public handle to the driver file record. The callback runs with the original
stack and registered GP. Its v0/v1 result is restored through a wide-value
continuation. Native return requests carry both result words; 32-bit services
continue to consume only v0. Manager errors sign-extend into v1. Invalid native
handles return EBADF even when no ISO mount is installed, while handles belonging
to another provider are left for that provider. No 64-bit cursor is simulated in
the manager; the compiled driver's own implementation supplies the semantics.

Extended directory opens set FIO_O_DIROPEN (8). Reads allocate a 384-byte,
64-byte-aligned iox_dirent buffer through the same native heap helper used for
file records. The allocator currently provides 256-byte alignment. The buffer is
zero-initialized and cannot overlap the legacy destination. Positive callback
results convert the first 40 stat bytes, including extended type/access bits,
and copy the 256-byte name with bounded strncpy-style zero padding. The legacy
privdata word is preserved, matching the public wrapper's field selection.

Temporary storage is released on entry, EOF and error returns. EOF/error calls
leave the caller output untouched instead of exposing uninitialized temporary
fields. Completion validates the original heap arena and output again; a fault
retains the saved result and allocation so retrying does not invoke dread twice.
Allocation failure returns ENOMEM before the callback and does not advance its
cursor. Unknown extended type encodings remain unsupported.
