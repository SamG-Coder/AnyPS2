# EE LLVM native conversion

AnyPS2 converts a bounded, statically discovered EE instruction set into LLVM IR
and compiles it ahead of time. The generated executable links native arithmetic
and startup services; it does not link the R5900 decoder, diagnostic interpreter,
PCSX2, or device models. GT4 visible boot and gameplay remain unestablished. The latest larger local
startup region emits and compiles successfully; its native execution reaches
an undiscovered command callback after 3,802,999 operations
with an explicit private copied-payload plan. The run includes native semaphore
creation, callback registration, table introspection, native timer initialization,
cache services, application settings, thread creation, ID lookup, priority change and execution of the
ahead-of-time compiled copied payload.
Native event delivery and public INTC handler chains are implemented; gated timer
modes and reset-on-compare remain unfinished. The expanded private payload entry
plan now compiles address-space routines into native requests; no production
mapping provider is connected. The latest run registers these routines without
executing a mapping request, initializes the native SIF receiver, and reaches DMA
handler registration and enablement, then exchanges command-buffer initialization
with the native bootstrap peer, receives the RPC initialization reply and enters
its compiled SIF0 handler, resolves its uncached buffers and rearms reception.
The next missing ahead-of-time target is a command-table callback.

## Conversion and execution boundaries

`nt_ee_codegen` accepts at most 262,144 sorted decoded instructions. It validates
instruction metadata before emitting output and groups unsupported operation
families with occurrence counts and first addresses. For unknown encodings, the
report identifies the primary opcode and applicable function/format/subgroup.
This inventory covers discovered code, not every byte in an ELF segment or every
possible indirect destination. Invalid selectors and malformed control flow also
reject conversion. Failed validation emits no partial LLVM project.

Generated functions contain at most 512 instructions, retaining each branch with
its delay slot. A balanced dispatcher routes transfers between compiled functions
without recursive host calls. Each function uses exact compiled PC destinations;
holes, unaligned targets and undiscovered code stop as `unmapped_code`. There is
no runtime instruction fetch, decode, interpretation, JIT, or fallback.

An operation budget permits resumption at instruction boundaries. Successful
instructions retire once. A syscall, trap, breakpoint, numeric failure, or service
wait retains its PC and does not retire. Pending branch targets and delay-slot
origins survive pauses. State storage shares `ee_state.hpp` with diagnostic
consumers without linking their execution machinery.

## Integer and packed operations

Scalar word results sign-extend to the low 64-bit destination. Scalar writes
preserve the upper 64 bits. Integer register zero supplies zero and ignores writes.
Inputs are captured before aliased destination updates. LLVM operations omit
incorrect overflow promises and never execute an out-of-range shift or an unsafe
division.

| Family | Lowering and supported behavior |
| --- | --- |
| Integer arithmetic | Word/doubleword add, subtract, immediate and register forms; signed-overflow forms stop before mutation; wrapping forms use modular arithmetic. |
| Logical, compare and move | Scalar bitwise operations, signed/unsigned comparisons, MOVZ/MOVN, and immediate forms. PLZCW counts leading sign bits in two words. |
| Shifts | Immediate and variable word/doubleword shifts; variable counts are masked to their widths. |
| Multiply/divide | MULT/MULTU/DIV/DIVU and secondary-bank forms; MADD/MADDU and secondary forms share one lowering. Both accumulator words sign-extend, including unsigned results. Divide-by-zero and INT32_MIN/-1 are explicit integer cases. |
| Packed arithmetic | Byte/halfword/word wrapping and signed/unsigned saturating add/subtract, signed greater-than and equality masks, halfword/word min/max and saturating absolute value, and PADSBH. |
| Packed shifts | Halfword/word fixed shifts and two-word variable shifts with sign-extended doubleword results. |
| Packed data | Bitwise operations, PCPY variants, byte/halfword/word pack and interleave, color packing, PEX/rotation permutations, PINTH/PINTEH. |
| Packed products | PMULTW/PMULTUW, PDIVW/PDIVUW, PMADDUW, PMULTH/PMADDH/PMSUBH, PHMADH/PHMSBH, and PDIVBW. Shared integer primitives avoid duplicated divide semantics. |
| Accumulator transfers | Scalar and packed HI/LO transfers; PMTHL preserves untouched words; PMFHL supports LW/UW/SLW/LH/SH packing and saturation. |
| Funnel shift | QFSRV uses an LLVM funnel intrinsic for the documented 0..15-byte SA domain. An arbitrary MTSA value outside that domain stops before mutation. |

MTSA stores the source low word in the byte-count representation observed by
MFSA. MTSAB/MTSAH produce bounded byte counts. Packed signed word
multiply-accumulate (PMADDW/PMSUBW) remains unsupported: public references describe
special multiplication behavior which the ordinary full-product path does not
claim to reproduce.

Packed operations use LLVM vector arithmetic, saturation intrinsics, comparisons,
selections and shuffles. Halfword products preserve the documented arrangement of
all eight accumulator words, including PHMSBH's complemented odd product words.
No numerical performance improvement is claimed without measurement.

## Control flow and native events

J/JAL/JR/JALR, integer conditional branches and likely variants, conditional-link
branches, and BC1F/T with likely forms use compiled control flow. The branch
condition and target are captured before link writes. Untaken likely branches
skip the delay instruction, including its memory checks and retirement. Branches,
syscalls, and ERET in delay slots reject conversion.

ERET selects ErrorEPC while ERL is set, otherwise EPC, and clears only the selected
exception-level bit. It performs an immediate compiled transfer without a delay
slot. An unavailable target stops after the return operation retires. This is
continuation metadata handling, not BIOS exception dispatch or virtual memory.

TGE/TGEU/TLT/TLTU/TEQ/TNE and their immediate forms compare low 64-bit values.
Immediate operands sign-extend for both signed and unsigned comparisons. A true
predicate returns `trap` before retirement and preserves delay-slot context; a
false predicate continues. BREAK and signed arithmetic overflow similarly return
explicit unretired stops. No exception handler is synthesized.

EI/DI preserve their privilege conditions and modify EIE only. Native producers
can publish pending interrupt classes in bits 8..15 between runner calls. The
compiled boundary checks IE, EIE, exception levels and class masks. Delay slots
finish before delivery. An enabled event returns `interrupt_pending` without
clearing it or retiring the following instruction. Native event producers and
handler routing are not yet connected.

MFC0/MTC0 expose status, EPC, ErrorEPC, and disabled performance metadata. Status
reads use the public visibility mask. Performance writes/readback never invent
cycles; active measurement or enabling counting stops as `unsupported_system`.
Nonzero MTPS register fields retain their public ignored-write behavior.

## RAM and ordering

The initial ABI requires a little-endian 64-bit host. Scalar byte/halfword/word/
doubleword loads and stores use a caller-owned RAM span. Addresses wrap to 32 bits;
KSEG0/KSEG1 aliases and the native application uncached RAM windows resolve to
physical RAM. Alignment, null storage, the complete
access extent and the 32 MiB RAM limit are checked before access. Integer-zero
loads still check addresses. No load-delay model is introduced.

LQ/SQ round down to a 16-byte boundary. LWC1/SWC1 preserve raw FPR bits. LQC2/SQC2
currently require 16-byte alignment and use the same RAM checks plus vector
ownership checks. Misaligned vector transfers remain explicit failures.

LWL/LWR/SWL/SWR and LDL/LDR/SDL/SDR share bounded merge lowering for the containing
aligned word. Unselected bytes survive. LWL sign-extends its result; a partial LWR
preserves bits 32..63, while a full LWR sign-extends. Merge stores check the entire
read/modify/write extent, conservatively rejecting overlap with executable bytes
even where a partial store would leave those bytes unchanged. Paired instructions
commit independently across aligned-word boundaries.

All stores check sorted, merged protection ranges covering compiled addresses and
all executable ELF segment extents, including undiscovered code. Failed memory
operations preserve registers, RAM and continuation state. `fault_address` retains
the original effective address for `memory_fault` or `code_write`.

SYNC modes 0 and 16 become sequentially consistent LLVM fences. Supported data
cache hit operations (DHWBIN, DHIN, DHWOIN) validate a RAM address and use the same
native ordering fence: this RAM model is coherent and has no separate dirty cache.
PREF is a non-faulting locality hint left to the native optimizer. These operations
do not complete DMA, invalidate generated native code, or acknowledge a peer.
MMIO, scratchpad, cache data/tag writes, and virtual-memory behavior remain
unsupported by this direct RAM path.

## Scalar floating point

Pure arithmetic lives in `ee_float.hpp`; diagnostic and native callers reuse it.
Fixed-width C entry points keep generated IR independent of C++ struct-return ABI.
There is no runtime instruction dispatch in these helpers.

- ADD/SUB/MUL and ADDA/SUBA/MULA implement the documented finite chop model with
  integer significands, signed zero and denormal-input flushing. Results and
  current/sticky underflow flags commit only after successful validation.
- DIV uses explicit nearest-even integer division, zero-denominator saturation,
  and current/sticky invalid/divide flags. SQRT computes an integer square root
  with explicit nearest rounding; negative finite operands use their magnitude
  and set invalid flags, while signed zero is retained.
- Exponent-255 arithmetic operands and extended-range arithmetic results stop as
  `unsupported_numeric` before modifying destinations or flags. These bounded
  helpers do not establish full PS2 floating-point compatibility.
- CVT.S.W uses integer chop conversion; CVT.W.S truncates and saturates to signed
  32-bit bounds, including exponent-255 inputs. Conversions preserve FCR31.
- MIN/MAX use raw signed-magnitude ordering, preserve selected input bits, and
  clear current overflow/underflow flags. MOV copies bits; ABS/NEG manipulate the
  sign and clear current overflow/underflow flags.
- Comparisons normalize exponent-zero operands and reject exponent-255 values;
  unconditional C.F clears the condition bit without inspecting numeric domains.
  Only the condition bit changes.
- MTC1/MFC1 transfer raw bits; FPR0 is writable. CTC1/CFC1 support FCR31 with write
  mask `0x0083c078`. Other selectors remain rejected; no processor identity is
  fabricated and no operation changes the host rounding mode.

Scalar accumulating multiply-add/subtract and reciprocal-square-root remain
unsupported. The finite arithmetic policy is documented rather than silently
substituting extended PS2 numbers with IEEE infinities or NaNs.

## Vector ownership and macro operations

`VectorStorage` is caller-owned storage shared with converted vector code. Its
ownership is cooperative between calls, not concurrent access synchronization.
Missing storage returns `unsupported_vector`; active storage returns
`vector_pending`. The production probe does not yet attach a vector owner.

QMFC2/QMTC2 transfer all 128 bits after ownership validation, including their
interlocked forms. Integer zero and VF0 retain constant/ignored-write semantics;
access to zero registers still observes synchronization. VF0 supplies
`(0,0,0,1.0)`. LQC2/SQC2 share that contract.

VADD/VSUB/VMUL and broadcast forms call fixed arithmetic primitives with operands
resolved at conversion time. Selected lanes, MAC and status commit together only
when every selected lane is supported. The common pure vector evaluator snapshots
aliased operands, preserves unselected destination lanes, and reuses scalar finite
arithmetic. Extended numeric domains remain explicit failures.

VMOVE/VMR32, VABS, VMAX/VMINI and broadcasts, and VITOF/VFTOI scales 0/4/12/15
preserve unselected lanes and flags. Min/max use integer ordering of raw floating
bits. Conversions use exact power-of-two scaling with chop/saturation rules.
VNOP observes the ownership boundary without modifying vector state.

CFC2/CTC2 use typed native callbacks with conversion-time selectors and wait flags.
The owner reports ready, pending or unsupported. Reads sign-extend; pending and
unsupported writes commit nothing. Random-register CFC2 remains rejected because
its special transfer semantics are not supplied by this adapter. No callback
receives a guest opcode stream. Full vector microprogram conversion and the
production owner remain separate unfinished work.

## Startup services and project output

SetupThread validates the shared initial-stack plan, argument buffer and generated
code-protection predicate before publishing main-thread metadata and an explicit
empty launch context. SetupHeap owns a bounded extent below that stack; size -1
extends to the stack base. EndOfHeap reports the accepted end. Rejected calls keep
the syscall unretired. These services share planners with diagnostic consumers;
they do not provide an allocator, full scheduler, or EE/IOP connection.

The native service owner also provides a bounded semaphore table (256 live
handles), CreateSema/DeleteSema, SignalSema, PollSema, uncontended WaitSema, and
status queries, including their supported positive interrupt-call numbers. Counts
and handles belong to each runner instance. Status writes validate their complete
extent and code protection before committing; allocation and failed accesses are
transactional. Invalid handles, exhausted tables and empty polls return -1 with
64-bit sign extension. Status preserves the caller's init_count field.

Contended waits and signal overflow stop at the syscall boundary without mutation;
no thread is silently blocked or resumed, and overflow error semantics remain
unestablished. Thread scheduling, interrupt-context restrictions, waiter ordering,
and negative syscall aliases still require implementation. Stored attributes and
options are reported back but do not establish scheduling support.

Discovery follows startup syscall continuation candidates when register 3 is
assigned a recognized constant immediately before the call. Runtime dispatch
still validates the actual number and arguments. This rule is generic; other
call-number constructions and unknown indirect targets remain discovery gaps.

Project generation writes LLVM, initialized private data, a CMake project and a
conversion report into a new directory. The current build path targets Windows
x86-64 with Clang and MSVC. The executable verifies data size/fingerprint, requires
no source ELF at runtime, and reports explicit stops. Imported game content and
all generated game artifacts belong under ignored `work/` and must not be committed.

## Implementation and verification

`llvm.cpp` owns region validation, partitioning and control flow. Cohesive lowering
modules cover memory, packed operations, integer and packed products, floating
point, vector operations, traps and system transfers. `llvm_writer.hpp` supplies
shared SSA, register, ownership and scalar-instruction helpers.

The combined synthetic EE execution fixture exercises these families through
compiled LLVM. Tests cover numerical boundaries, register aliases, zero registers,
lane masks, accumulator layout, branch delays, budgets, failed accesses and
unavailable native owners. Project tests verify generated compilation and linkage
without interpreter dependencies. Run affected milestone tests after a cohesive
batch; individual opcode edits do not require repeated integration runs.

### Native memory initialization contract

The converted program uses flat coherent RAM, with no guest TLB reservations or
private console cache lines. MTC0 Wired accepts a zero low word; nonzero requests
stop transactionally as unsupported system behavior. MFC0 Wired reports zero.
DXLTG returns the empty-tag contract through MFC0 TagLo (zero valid/dirty/address
bits). Indexed writeback/invalidate, instruction-cache invalidate and branch-target
flush lower to ordering fences. Their operands select console cache indices and
are not dereferenced as RAM addresses. Hit operations retain their RAM validation.

This deliberately replaces console cache residency with native coherent memory;
it does not reproduce cache tags, locking, cache-as-storage, or guest TLB mappings.
Tag/data writes and other unsupported selectors remain rejected. Code writes are
still rejected; flushing cannot install new executable content. Fences do not
complete asynchronous I/O or graphics work. Programs requiring nonempty cache-tag
observations need a separate conversion strategy; these operations do not prove
that such programs are compatible.


### Ahead-of-time syscall handlers

SetSyscall can register, replace or remove entries in an instance-owned 256-slot
table. Nonzero targets must be aligned RAM addresses present in the generated
instruction membership predicate; executable ELF protection alone is not enough.
Kernel RAM aliases normalize to physical addresses. Missing compiled targets stop
as unmapped code, with the requested syscall and handler in probe diagnostics.
The timer-table alias and negative syscall aliases remain unsupported.

Registered handlers take precedence over native services. Calls transfer directly
into generated LLVM, with at most 32 nested native frames. Entry validates the
16-byte stack reservation and protected writes before saving the caller's return
address, setting EXL and retiring the syscall. A reserved return gate restores the
stack, return address, continuation and prior EXL; it executes no guest code.
Budget pauses preserve active frames, including pauses at the return boundary.
Malformed return stacks stop without popping a frame. Other status bits and the
handler's result remain visible.

Discovery follows SetSyscall's continuation and proposes callbacks at direct
calls to simple syscall wrappers. A shared 128-step backward-slicing budget per
argument bounds work. Supported arithmetic and loads from the initialized ELF
recover table-backed candidates, including delay-slot arguments and saved-register
ABI candidates across calls. Candidate addresses must be aligned, file-backed
executable locations. This is over-approximate compilation discovery, not proof
that writable table contents or saved registers remain constant. It never replaces
runtime loads or registration checks. Discovered registration numbers also enable
compilation of their syscall continuations. Dynamic tables, indirect wrappers and
more complex argument construction remain discovery gaps. Registration never triggers runtime decoding,
compilation, or execution of RAM bytes. Callback discovery must supply the missing
code before a registration can succeed.


### Native registration metadata

Project generation reserves 1 KiB of RAM for a guest-readable view of the 256
native callback registrations. Placement starts at a native-owned low-RAM
candidate and advances past all ELF extents, including BSS; it never overlaps
loaded program storage. Failure to reserve an extent rejects conversion. The
project records the address as a build definition, independent of game identity.

Successful SetSyscall writes the supplied pointer into the corresponding metadata
slot; removal clears it. This exposes original native service data to converted
programs that inspect their registrations. It contains no firmware instructions,
BIOS table reconstruction, or placeholder addresses for unimplemented services.
Dispatch still uses normalized, verified compiled targets. Service entry validates
the entire published view against its owner; unexpected guest changes stop as
unsupported system behavior rather than silently changing dispatch. This is a
readable metadata contract, not support for arbitrary kernel-table patches.

The probe accepts an optional fourth argument `--trace-services` after the data
path and budget. It prints syscall boundaries and four argument values to stderr,
without instruction decoding. Keep traces from commercial programs local and
ignored. Trace output is diagnostic, not a compatibility result.


### Native polling timers and I/O access

Aligned integer LW/LWU/SW accesses outside RAM can call a typed native 32-bit I/O
provider after kernel-alias normalization. Reads commit a destination only on
success; failed accesses preserve PC and operation count. Missing providers,
unknown addresses and unsupported widths remain explicit stops. This boundary
passes addresses and values, never instructions, to native services.

The probe attaches native polling timers backed by `std::chrono::steady_clock`.
COUNT uses the public timer units and prescalers, with 16-bit wrapping; mode
changes snapshot elapsed time, and stopped timers retain their count. Tests inject
a clock rather than sleeping. Configuration/target/hold storage belongs to the
native timer owner. Gated/HBlank timing and compare-reset modes are rejected
before mutation; compare and overflow interrupts require an attached native event
owner. There is no instruction-count time approximation,
console bus-cycle scheduler or synthesized interrupt completion. Host scheduling
and elapsed wall time can differ from console execution timing.


### Native cache-service boundary

FlushCache and the public negative iFlushCache entry accept modes 0 through 3 and
apply a sequentially consistent host ordering fence. RAM is coherent, and output
instructions are immutable LLVM-compiled functions. The service preserves v0
(the API returns void), retires once, and rejects other modes without mutation.
It neither completes pending I/O nor publishes copied instructions. An indirect
transfer or registration naming uncompiled RAM still stops after a flush. Any
copied executable payload must be discovered and compiled ahead of time with a
separate validated publication contract.


### Copied payloads compiled ahead of time

`ps2-recompile <boot.elf> <output> --payloads <mapping-file>` accepts a local,
explicit payload plan. Each nonempty line starts with three decimal or `0x` numbers:
source ELF virtual address, destination RAM address, and byte size. Optional
additional numbers name aligned entry offsets within that extent. The destination
start is always an entry; explicit offsets cover argument-dependent dispatch
that static candidate analysis cannot resolve. Destinations
may use cached/uncached kernel aliases. Sources must be aligned and fully
file-backed; destinations must be aligned, bounded and disjoint from every ELF
extent, other payloads and reserved service metadata. No title-specific plan is
built into AnyPS2. The output retains `payloads.txt` for reproducibility.

The converter discovers payload instructions at their destination addresses and
compiles them into the same LLVM executable. It can add statically materialized
return-pointer candidates within mapped payloads. Input bytes are not rewritten:
this supports payloads already prepared for their target location, not arbitrary
relocation or unknown pointer fixups. Source data remains in the ordinary initial
image; destination bytes are populated by the converted program's own copy.

Payload RAM remains writable for initialization. Before each compiled payload
instruction, generated code compares the complete mapped extent with the expected
input bytes. Missing, partial or modified content stops as `unpublished_code`
without retiring that instruction. This comparison is a publication guard, not an
instruction decoder or JIT. Original ELF executable ranges keep their existing
write protection. Full-extent checking deliberately prioritizes correctness;
it is not the final performance design. Writable data inside a mapped payload
will also invalidate it, requiring a narrower code/data publication contract.

The runtime never compiles a new version on mismatch. Registered addresses must
still be members of the generated instruction set. Explicit plans currently need
local analysis; automatic identification of arbitrary copy routines, dynamic
payloads and fixups remains unfinished.


### Native interrupt ownership and delivery

The native platform owns a cooperative pending-event set and enable mask. Producers
raise lines between compiled execution slices; no events are fabricated from an
instruction count. The INTC status/mask compatibility view acknowledges requested
pending bits and toggles requested enables, and native I/O republishes readiness
before the next generated instruction. Other interrupt classes remain untouched.

SetSyscall's Timer 3 alias (0x12c) registers a verified compiled callback for line
12 and enables that line. Missing owners, unknown code, invalid targets and
registration during an active interrupt remain explicit failures. This alias is
not part of the ordinary syscall-pointer metadata table.

On an eligible generated interrupt boundary, the runner selects a pending enabled
line in ascending order, validates its callback and stack reservation, and saves
the full interrupted state in native-owned storage before acknowledging that line.
It enters compiled handler code with EXL set and a reserved native return gate.
Return restores the interrupted state while retaining retired operation count.
Budget pauses preserve ownership. Unhandled lines, unavailable stacks, unsupported
interrupt classes, nested interrupts and interruption of active syscall frames do
not consume pending events. A syscall may run inside a handler, but an unfinished
syscall frame prevents interrupt return.

Native compare/overflow event production is connected through the platform owner.
Timers accumulate elapsed monotonic time and fractional ticks arithmetically;
there is no per-tick loop. Compare and overflow status flags latch and suppress
repeat notification until software clears them by writing one. INTC event
acknowledgement and timer status acknowledgement are separate operations.

The runner polls native producers between chunks of at most 4,096 compiled
operations and at native I/O boundaries. This is a polling-latency bound, not a
conversion from instructions to time. A loop without I/O therefore still permits
native deadlines to become visible. Fake-clock tests verify event thresholds,
flag rearming and actual compiled-handler execution without sleeping. Wall-clock
scheduling differs from console timing and is not claimed to be cycle accurate.


### Public INTC handler chains

AddIntcHandler stores up to 64 native-owned records with reusable positive IDs,
line, compiled target, callback argument and GP. Insertion supports head (0), tail
(-1), and before an existing ID. Registration does not implicitly enable a line.
RemoveIntcHandler validates line/ID ownership and returns the remaining count for
that line. Invalid handles, positions, lines and exhaustion return -1; malformed
or uncompiled targets stop without installing a record.

EnableIntc/DisableIntc and their public negative interrupt-safe forms change line
readiness idempotently and report whether it changed. Delivery snapshots a line's
ordered records, followed by its alias callback if present. Each callback receives
its saved argument and GP. A negative return skips the remaining callbacks for
that line; otherwise delivery proceeds through the snapshot before restoring the
interrupted state. Registration/removal during active delivery remains rejected.
Simple AddIntcHandler wrappers now contribute static callback roots to conversion.

### Native thread lifecycle

The LLVM runner now owns bounded native thread records and cooperative compiled
continuations. CreateThread validates the public nine-word descriptor, compiled
entry, priority and stack bounds before allocating a dormant record. StartThread
initializes the worker argument, GP, stack reservation and compiled root return.
DeleteThread accepts only dormant noncurrent records. GetThreadId,
ChangeThreadPriority and both thread-status queries use the same owner.

A completed thread service can switch to a higher-priority ready continuation.
ExitThread makes the caller dormant and selects a ready successor. If no successor
exists it stops explicitly. Context switches preserve thread register state while
keeping the operation budget and CPU control/exception state shared. Switching
inside an active syscall-handler or interrupt frame remains unsupported. Sleep,
wakeup, suspension, ready-queue rotation and contended semaphore waits are not
implemented in this owner. These are cooperative native continuations, not host
threads running guest memory concurrently.

Conversion discovers candidate worker entries from simple CreateThread wrappers,
initial descriptors and local descriptor stores. Stack-pointer moves and offset
forms are supported, including stores after argument setup. Candidates must be
aligned and file-backed executable addresses; runtime registration still verifies
exact compiled membership. The bounded analysis never supplies runtime constants
or decodes new instructions during execution. An undiscovered worker stops with
an explicit uncompiled thread entry diagnostic.

### Native application preferences

Set/GetOsdConfigParam and Set/GetOsdConfigParam2 use application-owned settings.
`ApplicationSettings` can be supplied to `StartupServices`; each service instance
owns an independent copy. Defaults are English, 4:3, UTC, digital audio enabled,
24-hour time and year/month/day dates. They are documented application defaults,
not settings read from firmware, a console, or the host operating system.

The primary public ABI word and four-byte extended format retain values written
by compiled application code for that instance's lifetime. Extended operations
support bounded byte ranges, including empty ranges. Reads validate the entire
output buffer and code protection before writing; invalid requests do not partially
change memory or preferences. These void services preserve v0 and retire exactly
one operation. There is no persistence, firmware lookup or NVRAM implementation.

### Address-space request compilation

The converter recognizes CP0 Index, EntryLo0/1, PageMask and EntryHi transfers,
and TLBR/TLBWI/TLBWR/TLBP. It lowers these into compile-time selected native
address-space requests with explicit operation, parameter and value arguments.
No instruction encoding is passed to the provider and no runtime decoding occurs.
This permits registration of compiled routines whose mapping paths may not run.

A production mapping provider is not connected. Executing these requests without
one stops with `unsupported_system`, preserves the destination and PC, and retires
nothing. A provider must implement address-space behavior consistently across
compiled memory accesses, executable targets and native services before installation;
returning success alone is not sufficient. Existing flat RAM and direct aliases
remain the only production memory layout. Wired reservations are still rejected.
The synthetic recorder verifies the request ABI and failure/retry boundary only,
not functioning TLB mappings or application address-space switching.

### Native transfer completion status

A native completion owner records explicit producer notifications for ten transfer
channels. Its public DMA status view exposes latched completions and enabled
notifications. Low-word writes acknowledge selected events; high-word writes toggle
notification enables. Mask changes never create or discard completion events.
Generated loads/stores use the existing native I/O path, including address aliases.
The runner and I/O boundaries publish the DMA interrupt class without modifying
other interrupt classes.

No production transfer producer is connected yet. DMA handlers share the native
interrupt dispatcher described below; an enabled completion with no registered
handler stops at an interrupt boundary with its event preserved. Completion tests explicitly raise events from a synthetic native
producer; startup status reads begin empty because no work has been submitted.
Native SIF0 packet delivery is described below; outgoing submission and other
channel controls remain incomplete. No DMA timing loop or
instruction-count-based completion is introduced.

### Native SIF receive and link coordination

The EE native packet destination supports a configured normal receive extent and
whole-packet chain delivery. A native producer supplies payload bytes and the
chain destination/end indication; the endpoint does not fetch DMA tags or clock
a FIFO. It validates the entire destination and original-code protection before
copying, updates the visible receive extent, and raises channel 5 completion only
after terminal delivery or an enabled tag-interrupt termination. A nonterminal
packet leaves reception armed. Active restart, active extent edits, unsupported
modes and partial preloaded chain state stop explicitly. Failed delivery leaves
memory, receive state and completion events unchanged.

SifSetDChain arms the local receiver and publishes local readiness, while
SifStopDma stops reception. These void services preserve v0. SifSetReg/SifGetReg
share application-owned command addresses, flags and three software slots. Only
an explicit peer publication supplies peer address/readiness; setting local
readiness does not create a peer. Stop does not discard already visible payloads
or latched completion events. The native IOP session is not yet wired to this EE
endpoint. The outgoing native batch interface below still needs a production peer.

### DMA completion handlers

Add/RemoveDmacHandler and Enable/DisableDmac (including the public negative enable
and disable aliases) use the same native callback registry and context machinery
as INTC. Each domain has independent positive handles and a 64-record limit;
DMA channel IDs are bounded to ten. Static direct-wrapper discovery now includes
DMA callbacks. Registration never compiles code at runtime.

A completed native transfer publishes the DMA interrupt class. Eligible DMA events
have priority over INTC events. The dispatcher snapshots the selected channel's
ordered callbacks, validates stack storage, and acknowledges only that channel
after entry is safe. Missing handlers or an invalid stack preserve the completion.
Callback arguments, GP, negative-return chain termination, budget pauses and full
context restoration share the existing implementation. Other pending events stay
latched for subsequent delivery; nested handler entry remains unsupported.

### Native outgoing SIF batches

SifSetDma snapshots a bounded array of up to 32 descriptors and their padded payloads
into native-owned storage before returning a transfer ID. The total snapshot is
bounded to one MiB. Source bounds/alignment, peer destination bounds and supported
attributes are checked across the entire batch before publication. A malformed
later descriptor cannot publish earlier transfers. Scratchpad sources remain
unsupported. Descriptor and payload changes after submission cannot change the
queued snapshot.

An explicitly connected native sink receives the immutable batch between compiled
execution slices. It must atomically accept all transfers or leave its state
unchanged. Backpressure keeps the batch pending; a failure stops the runner without
retiring instructions and retains the snapshot for retry. Only acceptance clears
the batch and raises channel 6 completion. SifDmaStat reports pending versus
completed/unknown IDs, and submission while busy returns zero. The sink context
must outlive the outbound owner; it cannot be replaced while a batch is active.

Submission without a connected sink stops explicitly; this interface itself does
not publish peer readiness. The probe now installs the limited bootstrap command
peer below. Integration of converted IOP modules and broader services remains
unfinished.

### Native bootstrap command peer

The EE probe now connects an original native command service to its outgoing sink
before publishing peer transport/command readiness. Its configurable peer address
is a message port, not a firmware address. It handles command-buffer publication
and bounded software-register updates from real submitted packets. Destination,
header length, padded extent and writable EE command-buffer bounds are validated
before committing command state. Completion is supplied by the outgoing batch
owner after acceptance.

This is a native replacement for bootstrap command services, not execution of an
IOP firmware image. It accepts one control packet per batch and rejects payload-
bearing, multi-descriptor, reboot and RPC call requests until their corresponding
native owners exist. RPC initialization and unavailable-server bind handling are
described below. Unknown requests remain pending and stop execution; they do
not receive fake success or a completion. Game-supplied IOP modules still require
ahead-of-time compilation and integration. The lower-level IOP native session is
not yet connected to this bootstrap service.

### RPC control replies and native launch context

RPC initialization replies travel through the native receive endpoint, raising a
real channel 5 completion after their bytes become visible. A stopped receiver
applies backpressure. Failed destination validation preserves the pending request
for retry and does not mark RPC initialized. The dispatcher handles bind queries
with a public unavailable-server reply; it currently has no registered servers.
The bind-reply encoder is shared with the existing native IOP RPC implementation.
RPC calls and service-backed replies remain incomplete.

The probe explicitly launches a native application context with global delivery
and INTC/DMA class masks available, while EIE remains closed until compiled code
executes EI. This is application startup policy, not a BIOS power-on register
image. Low-level State defaults remain zero for callers that supply their own
context. Tests execute EI with a real pending completion, then deliver and return
from the compiled handler. Arbitrarily enabling delivery after a stall is not
part of the runtime.

### Shared native RAM aliases and interrupt-safe SIF services

Compiled memory operations and native buffer services share the existing
`default_ram_address` policy. Application uncached addresses in
[0x20100000, 0x22000000) and accelerated uncached addresses in
[0x30100000, 0x32000000) resolve to the same physical backing. The reserved first
MiB and addresses beyond those windows remain unmapped. Bounds, alignment and
compiled-code protection apply after resolution, while failures report the
original application address. Cache timing and arbitrary TLB mappings are not
introduced. This policy is now defined once and reused by diagnostic RAM and
native service owners; LLVM emits matching range checks from the shared constants.

The public negative iSifDmaStat, iSifSetDma and iSifSetDChain aliases use the same
native implementation as their ordinary forms. Explicit registered overrides are
still checked using the original syscall number first. Compiled receive callbacks
can rearm the endpoint before returning, without replacing the interrupt context.
