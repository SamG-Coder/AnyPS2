# EE LLVM native conversion

AnyPS2 converts a bounded, statically discovered EE instruction set into LLVM IR
and compiles it ahead of time. The generated executable links native arithmetic
and startup services; it does not link the R5900 decoder, diagnostic interpreter,
PCSX2, or device models. GT4 produces its opening warning image at the selected
640 x 448 display size, with live Vulkan presentation during execution. Its
warning thread now wakes from native idle, signals the main thread and exits
through ExitDeleteThread. Native cleanup passes SBUS callback removal and GetMemorySize. Compiled debug
byte output reaches the host log. Static native layout initialization now completes. The next unsupported path is
ExecPS2 handoff into a secondary executable image;
the title screen and gameplay remain unestablished.

Object-table discovery tolerates independent argument loads between a table base
and callback slot load without folding mutable data. Native idle has a reserved
identity distinct from suspended application threads; it runs no console idle
code. Queries and mutations of that internal continuation remain unsupported.
ExitDeleteThread transfers execution to a ready continuation before releasing
its record. Retail semaphore counts may exceed their declared maximum, while
signed count exhaustion remains unsupported. Interrupt activity alone does not
reset the bounded foreground idle diagnostic timeout.

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
cycles; active measurement or enabling counting requires a native metric provider
and otherwise stops as `unsupported_system`.
Nonzero MTPS register fields retain their public ignored-write behavior.

Performance operations lower to typed native calls for control and counter
reads/writes. Providers receive execution-mode information and a candidate state;
failed requests leave stored counter state unchanged. Provider-owned external
state must also remain unchanged on failure. The default disabled-counter path
retains existing readback behavior. No instruction words reach this interface.
The probe identifies rejected control values and event selectors. The current
private startup requests events 1 and 6; their native measurement mapping remains
unfinished. A synthetic provider test is not evidence of PS2 timing equivalence.

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


### Runtime-built callback tables

Conversion now considers recoverable executable addresses stored by reachable
word-store instructions as additional compilation entries. This handles callback
tables initialized by application code, including stores in direct-call delay
slots. Slicing remains bounded and validates alignment and file-backed executable
extents. Dynamic values and volatile registers across calls are not guessed.
These are compilation candidates only: generated code retains the actual store,
load and indirect call, with no constant-folding assumption about mutable tables.

The private integration run executes the previously undiscovered command callback
and progresses to a second registered RPC callback after 3,803,412 operations.
That target is passed to a registration function rather than directly stored by
its caller. Interprocedural registration discovery remains unfinished. This is
native startup progress, not visible boot or gameplay.


### Registration functions and interrupt-safe semaphore calls

Direct-call analysis now summarizes which of the first four arguments a callee
stores. Summaries track exact register copies across supported operations and
branch paths, including likely-branch annulment and delay-slot stores. They are
bounded to 256 visited states, stop at unsupported operations or further calls,
and are reused per callee. Caller arguments use the existing bounded constant
slicer and shared executable-pointer validation. This discovers callback tables
populated through registration functions without replacing mutable table reads
or adding runtime decoding. Nested registration wrappers and arbitrary dataflow
remain outside this initial summary.

The native service dispatcher also recognizes the public negative-number aliases
for iSignalSema, iPollSema and iReferSemaStatus. They share the existing semaphore
implementation, preserve interrupt context and retain existing unsupported
contended-wait and overflow behavior. Raw registered syscall overrides still take
precedence over built-in alias normalization.


With these changes the private converted program executes the registered RPC
completion callback, signals its semaphore, returns from the interrupt and
consumes the completion in startup. A 100,000,000-operation bounded run repeatedly
binds to unavailable file-I/O service 0x80000001 (14 completion signals observed).
The peer reports that missing server in optional probe diagnostics. The next
integration requirement is a native file-I/O RPC server backed by the existing
disc service; returning a non-null server without that implementation would be
incorrect. No visible game boot or gameplay has been established.


### Native disc-backed file RPC

The native command peer now registers explicit service owners before connection.
A mounted ISO enables the legacy FILEIO server through the reusable read-only
`fileio::Service`; without a disc the bind reply still reports absence. Service
and buffer tokens belong to the native endpoint namespace, not firmware storage.
Payload-bearing calls accept one bounded argument packet followed by its command
packet. Sizes, destinations and reply mode are checked before invoking a service.

The file adapter validates result, read-buffer and optional edge-fixup extents,
including compiled-code protection, before changing handles. Receive readiness
and the completion destination are validated first. Backpressure leaves the
immutable outgoing request pending without opening a file or advancing a cursor.
Successful calls write result bytes directly into native application memory and
then deliver the completion packet and interrupt. Unaligned outputs preserve
adjacent bytes. Host paths are never derived from guest filenames.

Generated projects compile the existing file and ISO sources directly, with no
hardware model, interpreter, decoder or JIT dependency. The native executable
accepts `--disc <image.iso>`; optional service tracing reports completed requests,
opens, bytes read and the last method. Request statistics now count completed
service calls, excluding malformed calls that throw before completion.

With the private disc mounted, GT4 binds FILEIO and completes the version query,
then reaches another undiscovered indirect target after 3,805,328 operations.
It has not yet opened or read a disc file. No visible boot or gameplay is claimed.


### ELF lifecycle tables

The ELF reader now records candidate entries from allocated `.ctors`/`.dtors`
sections and typed INIT_ARRAY, FINI_ARRAY and PREINIT_ARRAY sections. It validates
section/name-table bounds, word layout, correspondence with loaded file bytes,
and executable targets. Legacy sentinel/count forms and supported kernel address
aliases are handled explicitly. Stripped inputs without sections still work;
missing metadata does not trigger scanning arbitrary data for pointer-like words.

Discovery compiles these entries alongside ordinary reachable code. It neither
runs constructors itself nor changes their order: the original compiled startup
still reads the tables and invokes the functions. Unknown indirect targets remain
explicit stops. Optional probe diagnostics now include the return address when
an indirect destination is missing, to make subsequent discovery work actionable.


The private generated executable now runs the previously missing constructor and
continues through additional startup initialization. It reaches an unsupported
scratchpad-memory access after 3,807,087 operations. The next implementation gap
is owned native storage for that workspace and matching LLVM memory lowering.
FILEIO still reports one completed version query with no game file opens or reads;
visible boot and gameplay remain unverified.


### Native scratchpad workspace

Each native platform owns a zero-initialized 16 KiB heap buffer for the application
workspace addressed through the EE scratchpad range. LLVM memory lowering selects
that buffer and its bounded offset directly, alongside the main application RAM
path. Ordinary byte/word/doubleword/quadword accesses need no device callback or
instruction decoding. Existing alignment, merged-access and register-bank
lowering stays shared; bounds and backing-pointer checks precede native accesses.

Scratchpad offsets do not alias protected main-memory code addresses. Main RAM
code protection is unchanged. Platforms cannot be copied because memory views and
callbacks refer to their owner. This is native data storage only: it does not
introduce scratchpad execution, DMA channels or a timing model. Native service
buffer APIs retain their existing supported address spaces unless explicitly
extended; direct compiled access does not imply every service accepts scratchpad
pointers yet.


With native scratchpad storage, the private converted program advances to GS CSR
access after 3,808,495 operations. The next boundary is native graphics reset and
initialization connected to Vulkan; the diagnostic hardware implementation is
not linked into the generated executable. FILEIO still has one version query and
no game file reads. Visible boot and gameplay remain unverified.


### Native graphics reset and 64-bit service access

LLVM LD/SD lowering now supports atomic 64-bit native service calls. A failed
read does not update its destination register; failure retains the instruction
boundary and original fault address. Wider access is not implemented by splitting
one operation into two independent 32-bit side effects. Event publication is
shared with the existing 32-bit service path.

The native graphics lifecycle accepts GS CSR reset/flush and the interrupt-mask
view. Reset and flush synchronize a real Vulkan device before completing; absent
or failed backends stop explicitly. The probe creates the renderer lazily on the
first synchronization request and reports initialization errors. Reset restores
the initial mask without clearing graphics memory. Status currently describes
an empty synchronous command stream, with no fabricated scanout or finish events
and no proprietary chip revision. Other GS operations remain unsupported.

The root project and generated executables share `libraries/graphics/vulkan.cmake` to build
the same Vulkan renderer and shader. Converted programs do not link the diagnostic
GS/hardware implementation. This milestone establishes device initialization and
synchronization only; it does not translate drawing packets or present a frame.


GsGetIMR/GsPutIMR and their public interrupt-safe aliases now use the same native
mask owner as direct accesses. Put preserves the caller's return register because
its API is void; Get reports the mask including reserved bits. This adds no scanout
interrupt producer. GS reset on the private converted executable was verified
against the local Vulkan device before advancing to subsequent startup calls.


The current private converted run initializes the local Vulkan device, completes
one GS reset and applies GsPutIMR, then stops at SetGsCrt after 5,968,757 operations.
The request is interlaced NTSC frame mode. Native display-mode/scanout setup remains
unimplemented; no game frame has been rendered or presented. FILEIO still records
only its version query, with no game file opens or reads.


### Native display-mode deadlines

SetGsCrt now configures supported interlaced NTSC/PAL frame or field modes after
backend synchronization. Invalid or unsupported modes and backend failures retain
the previous configuration. The native mode owner exposes SMODE2 consistently;
unsupported direct mode changes still stop instead of fabricating configuration.

Display deadlines use the same monotonic nanosecond clock abstraction as native
timers. Reduced rational arithmetic derives nominal NTSC (60000/1001 fields per
second) and PAL (50 fields per second) phase, blanking boundaries and field parity.
Elapsed intervals are folded arithmetically, without per-scanline stepping or an
instruction-count time source. Long pauses coalesce pending interrupt bits while
preserving current field parity. Clock rollback does not rewind state.

CSR sync flags latch until acknowledged. IMR changes can expose an already pending
GS event, and the platform publishes GS/VBlank-start/VBlank-end events through the
existing native interrupt owner. Reset stops display deadlines. These are logical
application display events, not evidence of physical monitor vsync or presentation;
framebuffer configuration, drawing translation and presentation remain separate
unfinished work.


The private generated executable passed SetGsCrt and reached VIF0 reset/control
access after 5,968,955 operations. Vulkan reset remained successful.

### Native VIF/GIF initialization

A shared native submission configuration owner now supports both VIF channels,
including reset, stop/break/cancel, configuration masks and row/column storage.
GIF configuration supports reset, pause and mode readback. FIFO input and command
execution remain explicitly unsupported; configuration never fabricates packet
completion or interrupts.

The preceding private executable passed VIF0 reset and reaches an unsupported VU
control read after 5,968,957 operations. At that milestone a native vector-control owner was not yet
connected. No game drawing or presentation has occurred; FILEIO still records
only its version query.


### Native vector ownership

The platform now connects a cooperative native vector owner to both compiled
control transfers and LLVM vector storage. Integer controls retain 16-bit values;
status and MAC reads use the same flags written by compiled macro operations.
Idle break/reset configuration supports both control bytes. Read-only controls
ignore writes. Unsupported selectors stop explicitly.

Active work blocks transfers and reset requests without mutation. A non-waiting
execution-status query can report active VU0 work; waiting queries remain pending.
No microprogram launcher or instruction decoder is introduced. The unused VU1
execution path has no work to report; future native launch support must extend
ownership and completion before accepting submissions.

The private executable now passes vector-control initialization and reaches VIF0
DMA access after 5,969,004 operations. Vulkan initialization succeeds, but no game
frame has been drawn or presented. FILEIO still records only its version query.


### Native packet submission boundary

VIF0, VIF1 and GIF submission configuration now feeds one native interface for
normal forward transfers. A consumer receives a bounded borrowed byte span and
channel identity. Success means the complete submission was consumed; any other
result must leave consumer state unchanged so the compiled store can be retried.
Only successful consumption advances the source, clears the count and publishes
the corresponding completion through the existing native interrupt owner.

The adapter validates alignment and complete RAM or scratchpad extents before
calling a consumer. Chain traversal and reverse transfers remain unsupported.
Idle channel configuration is retained as metadata. No production consumer is
connected yet, so transfer start stops explicitly; no completion is synthesized.
The platform and its attached Memory must outlive calls using the I/O interface.

Private GT4 execution passes the channel configuration accesses and now stops at
global DMA priority access after 5,969,128 operations. This run has not submitted
a native VIF/GIF packet or produced a game frame. Global DMA policy and conversion
of packet work remain the next integration tasks.


### Native submission policy

Global control enables native packet submission; the suspend bit prevents consumer
calls. Disabled or suspended starts stop without retirement or completion rather
than silently dropping work. Priority fields can be configured with arbitration
disabled. Enabling priority arbitration, MFIFO, stall control or cycle stealing
remains unsupported. Native calls serialize accepted work without a bus clock.
These controls apply to the packet adapter; native SIF services retain their
separate explicit transport and completion ownership.

The channel initialization slot at offset 0x80 accepts zero and rejects nonzero
configuration. A private rebuild caught its accidental rejection in the preceding
change; the compiled initialization regression now covers all three channels.

The latest private executable reaches its first VIF0 chain submission after
6,363,631 operations, with submission enabled. Chain conversion and a production
packet consumer are still required. The probe now reports the channel, source,
count, chain pointer and submission policy for packet-access failures. These
metadata diagnostics do not include game code or packet bytes.


### Bounded chain batches and native VIF configuration

The submission adapter now resolves REFE, CNT, NEXT, REF and END data descriptors
into one owned byte batch, with optional upper-tag data and interrupt termination.
Resolution is bounded to 256 descriptors and one MiB of output. RAM/scratchpad
extents are checked before a consumer is called. Cycles, malformed extents and
unsupported stall/call/return descriptors cannot partially submit work. Completion
and channel updates occur only after whole-batch consumption succeeds.

The platform now connects a native VIF configuration consumer. It supports NOP,
STCYCL, STMOD, MARK, STMASK, STROW, STCOL and idle FLUSHE. Configuration changes are
applied to a candidate state and committed together. Active compiled vector work,
interrupt-bearing commands, incomplete payloads, GIF input and other VIF commands
remain unsupported. This is data-protocol processing; it does not decode or
execute VU instructions and introduces no instruction-clock transfer engine.

The private GT4 batch now resolves to an unsupported MPG upload (command 0x4a).
It stops at the same 6,363,631 operations because the complete batch is rejected;
no preceding configuration command or completion is committed. The next task is
conversion-time discovery/compilation of uploaded VU microprograms and native
upload/launch ownership, not an interpreter or an unconditional successful upload.


### Offline VU upload input

MPG rejection now validates the full encoded payload length (zero count means 256
instruction pairs) before invoking an optional conversion diagnostic observer.
The observer receives a borrowed byte view, unit and destination; observing never
accepts the upload or commits the batch. The probe can explicitly capture this
input to a new private directory. No files are written without that option.

The separate nt_vu conversion library frames little-endian lower/upper pairs,
extracts operand fields and control flags, and distinguishes I-bit immediate data
from lower instructions. It validates VU0/VU1 capacity and rejects wrapping input
extents for now. ps2-vu-inspect inventories encoding families offline. Neither the
library nor the tool is linked into generated game executables. Operation semantics,
control-flow reconstruction, scheduling and LLVM lowering remain to implement.

The first privately observed chunk contains 256 pairs, 17 lower-word immediates and
12 end flags. This is an upload chunk, not evidence of a complete program or of 12
reachable functions. Startup remains stopped at MPG; this inspection does not
advance execution or establish a successful VU upload.


### VU control-flow candidates

The offline frontend now decodes typed direct, link, conditional and register
branches. Targets use signed pair-relative displacements and the selected unit's
address width. Discovery includes delay-slot pairs without treating each slot as
an independent fallthrough path. E-bit termination also includes its following
pair. Branch/end combinations whose ordering has not been implemented are reported
as complex delays rather than assigned guessed successors.

Calls seed both their target and a possible return continuation for compilation.
Indirect transfers remain explicit unresolved boundaries. Missing code is reported;
loops and repeated entries terminate analysis through a visited set. This is
conversion-time candidate discovery, not VU execution or a proof of reachability.

Inspection currently uses the supplied upload destination as an assumed entry.
For the private chunk, that assumption produces 11 candidate pairs and one unresolved
indirect transfer. Upload destination is not automatically the game's launch entry;
launch discovery must supply actual entry candidates before native binding. VU
arithmetic semantics, pipeline scheduling and LLVM lowering remain unfinished.


### Typed VU upper arithmetic

Upper decoding now produces explicit arithmetic operations, vector/I/Q/broadcast
operand selection, destination storage, register indices, lane masks and conversion
scales. It covers vector and accumulator add/subtract/multiply families, multiply
accumulate/subtract, min/max, outer products, absolute value, integer/float
conversion, clipping and NOP. Reserved encodings remain unsupported.

These records describe encodings; they do not implement floating-point results,
flag side effects, issue latency or executable lowering. The private chunk has no
unclassified upper words under this decoder. That is decoder coverage only, not a
claim that any VU microprogram can run. Lower operations and paired-instruction
ordering must be resolved before the resulting records can become native code.


### Typed VU lower operations

Lower decoding now records register banks, vector components, masks, signed or
unsigned immediate operands and explicit I/Q destinations. The initial families
cover integer arithmetic/logic, vector move/rotation, integer/vector transfers,
divide/square-root/reciprocal-square-root, WAITQ, literals and branches. Branch
records reuse the existing control decoder. Unsupported memory, flag and other
families remain explicit rather than being treated as NOPs.

The captured chunk now has no unclassified lower words: its inventory includes
literals, branches, integer OR, moves, division, reciprocal square root and WAITQ.
Integer OR to the constant register remains an integer operation in the IR; any
NOP simplification belongs in a later validated optimization step. Decoder coverage
does not establish paired read/write ordering, numerical flags or Q availability.
The runtime remains stopped at MPG pending LLVM compilation and native binding.


### Initial VU LLVM emission

ps2-vu-recompile emits a straight-line native function through typed operations.
It accepts contiguous terminated regions, including the E-bit delay pair, and
rejects branches, special control flags, numeric/Q operations and simultaneous
vector destinations whose priority has not been lowered. Missing operation families
are reported together before any output file is written. Existing output paths
are rejected.

The initial native ABI holds vector/integer data and I only; it contains no program
words or decoder. Lowering supports NOP/ABS upper operations, 16-bit integer
arithmetic/logic, immediates, vector moves/rotation and integer/vector transfers.
Both halves read their inputs before either half commits a result. Vector masks,
constant registers, signed transfer extension and integer wrapping are preserved.
This subset has no arithmetic flags or Q producer; their state and scheduling must
be added before numeric lowering. The ABI is not yet bound to EE upload/launch.

An original synthetic region has been emitted, compiled by LLVM and executed in a
native test executable. The private commercial chunk is still rejected with a
combined list of missing arithmetic, branch and Q families; no output is written.
No GT4 boot progress is claimed from the synthetic compilation result.


### Native VU comparisons and conversions

LLVM lowering now includes vector/broadcast/I min/max and ITOF/FTOI scaled forms.
It calls the existing fixed-width numeric helpers used by EE vector conversion;
no host float casts or duplicate rounding implementation is introduced. These
operations have no arithmetic-flag side effects. Masks preserve destination lanes,
and an I operand is read before a same-pair lower literal replaces I.

Generated objects using these operations link the shared numeric C ABI implemented
in libraries/ee_native/src/numeric.cpp, with runtime/include available at build time.
This is a numeric helper dependency, not the diagnostic CPU runtime. The native
fixture includes this dependency and executes actual emitted instructions. Finite
add/subtract/multiply with flag publication, accumulation, branches and Q producer
scheduling remain unsupported by VU lowering, so GT4's MPG input is still rejected.


### Finite VU arithmetic and transactional native regions

LLVM lowering now supports finite add/subtract/multiply with vector, broadcast
and I operands, including accumulator destinations. It reuses the existing finite
numeric C ABI. Shared MAC-lane and status assembly was factored out of the existing
vector helper so EE and VU paths use the same implemented flag policy. VF0 discards
results but still publishes arithmetic flags. Q operands remain unsupported.

The standalone native region ABI now returns Result and includes accumulator,
MAC and status storage. Generated code operates on a private working state and
commits the region only on success. Unsupported numeric inputs return
unsupported_numeric with the original input State unchanged, including changes
from earlier pairs. This transaction is valid for the current state-only regions;
future memory/device operations need their own commit and failure design.

The finite domain retains existing explicit rejection of nonfinite inputs and
unsupported overflow rather than substituting host arithmetic. Lower flag readers
and branch/Q scheduling remain unimplemented; final flag publication is not a
claim of cycle-accurate flag-pipeline behavior. The private input now reports only
multiply-add/subtract, branch and Q-related missing families, and remains uncompiled.


### VU multiply-accumulate lowering

MADD/MSUB and their accumulator destinations now use a shared finite numeric helper
that performs product and accumulation as separate operations. No host fused
multiply-add is substituted. Vector, broadcast and I operands share the existing
masked destination and flag path; Q still requires its pending-result model.

Nonfinite inputs, product overflow and intermediate product underflow remain
explicit unsupported numeric cases. The latter needs additional VU pipeline/flag
semantics and is not silently approximated as a zero product. Final-operation
underflow retains the existing finite helper behavior. Whole-region rollback
applies to all such failures. The private input's missing-family diagnostic is now
limited to lower branches and Q producers/waits/operands; upload and launch remain
unbound and no boot progress is claimed from this compiler milestone.


### Native VU branch blocks

The emitter now accepts a basic block ending in a direct, conditional, link or
register branch followed by its delay pair. It captures the target/condition before
the delay pair, executes that pair, and returns Result::transfer with next_pair in
native State. E-bit blocks return complete. Both paths commit only on success.
Branches inside delay slots and combined branch/E ordering remain rejected.

Integer branch operands retain a previous-write backup in native State, including
at a block boundary. Link targets are captured before link writes, and the delay
pair can observe the new link register. This code has no instruction stream reader
or interpreter fallback. A native compiled-block registry/dispatcher must validate
and resolve next_pair before this can be bound to runtime launches. Q and general
pipeline ordering are still incomplete; transfer success is not program completion.

A private 11-pair block ending at the first branch in the captured chunk was emitted
to LLVM and compiled to an x64 object. It was not executed or linked into GT4. This
verifies generation and compilation for that block, not its launch entry, dynamic
inputs, target resolution or complete upload coverage. All private artifacts remain
under ignored work/. Synthetic native branch tests establish the implemented ABI.
# Native VU block registry

`vu_dispatch.hpp` provides an owning, validated registry of native function pointers
indexed by VU pair address. Each registry represents one compiled program for one
unit; upload identity selection and launch binding remain separate unfinished work.
The registry contains no instruction words and performs no instruction decoding.
Transfers invoke only registered ahead-of-time compiled functions. Unknown targets
stop explicitly, preserving the next pair address for diagnostics.

Execution uses a caller-supplied block budget. Exhaustion preserves the continuation
for resumption; zero budget performs no work. Completed blocks remain committed,
while generated numeric failures roll back only the failing block. Block counts
include attempted calls that return numeric failure and are not cycle estimates.
Registry construction rejects duplicate addresses, null functions and addresses
outside the selected unit. Direct branch linking remains a future optimization.

## Synchronized native VU quotient operations

The LLVM backend lowers DIV, SQRT and RSQRT to fixed numeric helpers and stores
their pending quotient and current invalid/divide flags in the native ABI. WAITQ
publishes that result before either half of its pair reads Q. A subsequent producer
synchronizes an earlier result, and E-bit completion drains pending Q. Branches
preserve pending state across native functions. Upper arithmetic accepts Q operands.
Publication replaces current invalid/divide flags and accumulates their sticky bits.

Numeric helpers reuse integer-based finite division and square root, including
explicit exceptional-result handling for zero denominators and negative radicands.
Unsupported nonfinite inputs or overflow stop with block rollback. This is the
existing bounded finite numeric policy, not a claim of exhaustive VU floating-point
equivalence. Ordinary pair-count latency and dependency stalls are not implemented:
an upper Q read while a result remains pending returns `unsupported_schedule`, with
no state commit. This deliberately excludes unsynchronized Q reads, including cases
where enough hardware cycles would already have elapsed. No runtime opcode decoding
is added. The private upload now passes operation-family validation but still fails
the single-block boundary requirement; launch binding remains unfinished.

## Multi-block VU LLVM modules

`emit_program_llvm` discovers control flow from explicit entries and emits blocks
for roots, direct targets and conditional/call continuations. Each block retains
its delay pair. Overlapping entries can produce overlapping native regions, which
preserves entry semantics without a runtime decoder. Indirect transfers only route
to entries included in the compiled registry; unknown targets still stop.

One module contains helper declarations, native block functions and a constant
table compatible with `nt::vu::Block`. The generated `<symbol>_blocks` and
`<symbol>_count` accessors let the host construct the existing native registry.
All output is buffered until validation and lowering succeed. Missing pair
addresses and unsupported delayed transfers produce conversion diagnostics.

The private first-upload probe compiled 20 blocks from structural candidate
entries into an x86-64 object. Those entries are analysis candidates, not confirmed
launch targets. Including the remaining tail reports missing pair 256, beyond
that capture. The object has not been linked into GT4 or executed; collecting and
validating the complete upload sequence and native launch binding remain required.

## Full submission extraction

The opt-in probe capture now records the full rejected packet as well as its first
MPG payload. The production consumer still rejects it atomically and publishes no
completion. `ps2-vu-extract` is a separate offline tool: it parses known VIF framing,
skips payloads by length, collects ordered MPG chunks, and stops before commands it
cannot classify safely, including launches and interrupts. It validates extents and
truncation. Assembly applies uploads in order, with later pairs replacing earlier
pairs, and preserves holes as separate regions. Wrapping uploads remain unsupported.

The private first submission contains 256 pairs at destination zero and 80 at
destination 256. Assembling these resolves the previously missing pair. Conversion
of the earlier candidate entries now produces 21 native blocks that compile into
an x86-64 object. Additional structural entries in the second chunk expose a lower
operation gap; full-image conversion remains incomplete. GT4 still stops at the
same MPG submission after 6,363,631 operations. These objects have not executed.

## Native VU quadword memory operations

LQ, SQ, LQI, SQI, LQD and SQD now lower to LLVM loads/stores against a native data
array. Signed base displacements, lane masks, immutable VF0/VI0, pre-decrement,
post-increment, 16-bit index updates and the previous integer value used by delayed
branches are retained. Pair reads precede deferred writes. VU0 data wraps within
4 KiB; VU1 data wraps within 16 KiB. VU0 addresses selecting the VU1 register window
return `unsupported_memory` instead of accessing unrelated storage.

The existing block transaction now includes the native data array, preserving
rollback when a later access fails. This increases the potential block-copy cost;
no performance claim is made, and reducing transaction copies remains optimization
work. The array is program data only and contains no instruction decoder.

All currently identified structural entries in the private 336-pair image now emit
30 native blocks and compile to an x86-64 object. This closes the previously observed
lower-operation gap. The object has not executed; upload identity, launch binding,
runtime target coverage and unsupported Q scheduling remain integration concerns.

## Compiled upload binding

Multi-block modules now export image bytes, size, destination and unit alongside
their block table. These bytes are identity data only: the runtime compares them
exactly and never decodes or executes them. Commercial identity data belongs only
in ignored conversion output, under the same policy as generated LLVM and objects.

`vu_binding.hpp` owns the image identity and native Program, tracks uploaded pairs,
and rejects mismatched content, units and extents. It validates every chunk before
committing coverage. Launch requires complete coverage; reset removes that coverage.
Execution uses only the registered ahead-of-time functions and retains bounded
execution and explicit unsupported stops. This is currently one artifact per binding;
selection among different loaded images is not implemented.

NativePacketSetup offers an optional atomic upload-batch callback. It gathers MPG
views and validates all packet commands before invoking the owner. Failed ownership
checks leave packet configuration unchanged, and an unsupported later command never
commits earlier uploads. Without an owner, MPG retains its diagnostic stop. Synthetic
integration validates matching upload through native launch; the GT4 probe has not
yet registered a compiled artifact or connected its launch/state synchronization.

## Generated-project VU upload integration

New EE projects include the reusable VU CMake helper. `ps2-build --vu <module.ll>`
compiles and links one multi-block VU module into the executable; omitting the option
clears the cached module selection. The probe constructs a binding from its generated
identity/table accessors and uses the shared atomic packet adapter. The build links
native numeric services and the VU object, with no VU decoder, frontend or interpreter.
Generated game code remains LLVM and lives only in ignored private output.

The regenerated private GT4 project accepts its matching VU0 upload and reports
complete VU0 image coverage. Startup advances from 6,363,631 to 6,364,039 operations,
then rejects an unbound VU1 upload. Capturing that submission identifies eight chunks
forming a contiguous 2,046-pair image. No VU launch has executed, no game frame has
appeared, and the probe still needs native launch/state synchronization and support
for more than one linked artifact. The upload acceptance is not a boot claim.

## Native VU integer memory operations

ILW/ISW and their register-addressed forms ILWR/ISWR share the quadword memory
lowering. Loads select the low 16 bits of each enabled lane in x/y/z/w order,
with the last enabled lane determining the result. Stores zero-extend the source
integer into enabled 32-bit lanes. Signed immediate offsets, address wrapping,
VI0 and unmapped VU0 register-window rejection retain the same rules as the
quadword path. Integer loads do not create an arithmetic-style delayed branch
operand; a following branch observes the loaded value.

The private VU1 inventory now classifies 107 integer loads and 92 integer stores,
reducing unclassified lower operations from 312 to 113. These are inventory counts,
not execution evidence. Conversion from the broader structural candidate set still
stops at an unsupported lower family in the block starting at pair 9. The observed
upper outer-product/clip and remaining lower flag, graphics/control and extended
function families require further lowering; VU1 has not been bound or executed.

## Native VU launch inputs and outer products

XTOP and XITOP now read explicit 16-bit input offsets from the native state.
Separate validity bits prevent absent launch metadata from silently becoming zero.
Reading a missing input returns `unsupported_context` and rolls back the block;
VI0 destinations remain ignored. These reads do not create delayed arithmetic VI
backups. The EE/VIF launch owner still needs to supply the actual values.

OPMULA and OPMSUB lower xyz cyclic products through the existing finite multiply
and non-fused multiply/subtract helpers. They ignore the encoded destination mask,
preserve accumulator/vector w, preserve the w MAC flags, and update xyz flags and
status. Unsupported numeric inputs retain block rollback. This reuses the current
finite numeric policy rather than claiming exhaustive hardware numeric equivalence.

The private structural-candidate conversion now passes its earlier pair-9 stop
and reaches pair 533, which needs MFP and extended-function/P-register support.
This is a conversion diagnostic only: VU1 remains unbound and unexecuted, and GT4's
verified startup stop remains the VU1 MPG upload.

## Synchronized native VU1 P results

ERLENG lowers to a shared finite reciprocal-length helper, evaluating xyz squared
length, square root and reciprocal without using w. It uses the existing explicit
finite rounding policy, returns zero for zero length, and rejects nonfinite inputs,
intermediate overflow and intermediate underflow. This is a bounded implementation,
not exhaustive hardware EFU numeric equivalence.

The native ABI retains pending P across compiled branches. WAITP publishes it;
masked MFP copies the published bits into enabled vector lanes. A pending read
without supported synchronization returns `unsupported_schedule`. Another producer
or E completion drains pending P. No MAC/status flags are changed by these lower
operations. VU0 decoding still rejects these VU1 operations. Automatic P latency
and the remaining extended-function families are not implemented.

The private broader candidate conversion now reaches pair 601, where FMAND needs
MAC-flag visibility support. VU1 is still unbound and unexecuted. The compiled EFU
tests establish the supported arithmetic/synchronization path, not game boot.

## Guarded native MAC-flag reads

FMAND, FMOR and FMEQ now lower to native 16-bit integer operations. Their result
does not use the delayed arithmetic VI operand path. Each arithmetic flag writer
sets a four-pair visibility guard, carried across native transfers and advanced
before subsequent pairs. E completion drains the guard. A lower read in the same
pair as an upper writer sees the preceding flag state only if that state is ready.

This is deliberately a conservative visibility check, not a full FMAC pipeline.
When a writer is still pending, the read returns `unsupported_schedule` and the
block rolls back. The converter does not guess which older flags would be visible
or count unimplemented dependency/quotient stalls as progress. Once four later
pair boundaries have been reached without another writer, the latest flags are
safe to read. Implementing earlier reads requires richer dependency scheduling.

The private VU1 candidate conversion now reaches pair 767, where XGKICK requires
a native graphics-packet submission implementation. VU1 remains unbound and has
not executed; the verified GT4 startup boundary remains its upload.

## Native graphics request boundary

Ordinary VU1 XGKICK now terminates a compiled block with a graphics request and
saved continuation. The address is constrained to VU1 data storage. Program dispatch
and bound launches cannot advance while that request is pending. No instruction
fetch or decode is added. A consumer must explicitly accept the entire packet
before the continuation is released; absent or rejecting consumers leave it pending.

The shared graphics packet framer copies a bounded immutable packet from the data
ring, handling packed, register-list and image lengths, wraparound and end tags.
Extraction is limited to one traversal of the ring; oversized or unterminated
packets stop. This parses graphics data, not VU code. It does not interpret GS
commands, render primitives or reuse the diagnostic hardware GIF implementation.

This establishes a serialized native submission interface only. A production
Vulkan consumer and validation of asynchronous transfer/source-write ordering are
still required before claiming XGKICK compatibility. Kick-plus-E and delayed-control
combinations remain conversion errors. The private candidate conversion passes the
ordinary kick at pair 767 and stops at pair 768 with a specific kick-plus-E
delayed-completion diagnostic. VU1 remains unbound and no game graphics has rendered.

## Graphics request with E completion

XGKICK combined with E now includes its final delay pair in the compiled block.
The request captures the source address before that pair changes integer state.
Native execution remains pending until the consumer accepts the packet, after which
resumption returns complete without executing any further block or repeating the
delay pair. Starting a new bound launch clears the completion marker, but a pending
request cannot be bypassed by launching again. E completion drains pending Q/P and
MAC visibility state as in ordinary E blocks.

Delay-pair stores are rejected during conversion because changing outgoing data
requires transfer-order analysis. Other delayed-control combinations remain explicit
errors. The private candidate conversion now reaches pair 1120 and reports CLIP
plus clip-flag operations as missing. This does not connect a Vulkan consumer or
establish VU1 execution; the verified startup boundary remains the VU1 upload.

## Native clip history and flag operations

CLIP now computes its six comparison bits using integer bit-pattern comparisons,
including signed-zero and denormal-threshold behavior, and shifts them into the
24-bit history. It does not alter MAC/status flags. FCAND, FCOR and FCEQ produce
their boolean result in VI1; FCGET returns the low twelve clip bits to the selected
integer register; FCSET replaces the 24-bit value. Paired CLIP/FCSET remains rejected
until its write-priority semantics are implemented.

Clip writers share the conservative four-pair visibility guard used for MAC reads.
The guard survives native block transfers, and early readers stop atomically rather
than observing speculative flags. Flag history is retained independently of reader
visibility. Full stall-dependent flag timing remains unfinished.

The private 2,046-pair VU1 capture now emits and compiles from the identified
structural candidate entries into 392 native blocks. The x86-64 object remains
private and has not executed. This clears observed conversion-family gaps in that
capture, not runtime target coverage, scheduling, graphics integration or game boot.
The following integration milestone links separate VU0/VU1 artifacts; native game
launches remain unfinished.

## Independent native VU unit bindings

`ps2-build` accepts `--vu` and `--vu1` LLVM artifacts. Module generation uses
distinct exported symbols for each unit, and the generated project compiles both
objects ahead of time. A reusable upload router maintains separate bindings and
rejects missing units, duplicate registrations and mismatched image bytes.
Bindings validate artifact unit metadata before accepting uploads.

The private startup accepts both images and reaches an unsupported-system stop
after 6,364,155 operations. This verifies upload identity and coverage, not game
VU execution or boot. The linked inputs contain generated EE/VU objects, numeric
and native service objects, Vulkan and platform libraries; they do not include the
VU decoder, interpreter or JIT. Native launch/state handoff is still required.

## Transactional native VU1 launch

VIF1 MSCAL invokes the bound native module through `NativeVuLaunch`. BASE,
OFFSET and ITOP configure launch inputs; successful launches capture TOP/ITOP
and alternate the next buffer selection. Entry addresses use the unit address
mask. Only padding may follow a launch in the initial atomic submission path.
Mixed upload/launch batches remain rejected before either operation commits.

The owner executes a candidate state with a bounded native-block budget and
publishes it only on complete execution. Missing targets, numeric failures,
graphics requests and budget exhaustion retain the previous owner and packet
state. This serialized model does not establish asynchronous console timing.
VU0 macro/micro state sharing, UNPACK data preparation, continuation commands
and Vulkan graphics-request completion remain separate unfinished work.

Private startup now passes counter configuration without inventing measurements.
Counter reads and accumulation exist in the input; unavailable measurement reads
still stop. Replacing their values with constants or removing them is not justified.

The VU1 submission owner also accepts contiguous unmasked V4-32 uploads, with
32-bit lanes preserved as data. Destination wrapping and TOPS-relative addressing
are supported. Equal cycle lengths and normal row mode are required; unsupported
formats, masked writes and cycle/row transforms stop. Data writes and an optional
final launch share one candidate state, so a failed launch commits neither.
Mixed program-image and data uploads remain unsupported in one submission.

V4-16 and V4-8 now share a bounded data conversion helper with V4-32. Narrow lanes
are explicitly sign- or zero-extended without host signed-conversion dependence.
V4-5 expands packed 5:5:5:1 colour into 32-bit lanes and ignores MODE, as required
by that format. Transport padding is consumed separately from data, including odd
packed-colour counts. A zero encoded count means 256 vectors. Masked formats,
unequal cycle lengths and nonzero row modes for ordinary V4 formats still stop.
These are data conversions into native storage, not executable-code decoding.

## VU0 shared-state launch ownership

The native launch owner now supports VU0 with an explicit shared EE vector-storage
reference. Integer-register storage belongs to that same owner, so control
transfers and compiled microprograms see the same values. Launch imports current
vectors, integers, MAC and status flags; completion publishes those fields back.
Active ownership returns pending and failed native execution publishes nothing.
VU0 data uploads wrap within 256 vectors and its launch supplies ITOP without
VU1 TOP or double-buffer state. Unit routing selects independent native owners.

This is a cooperative serialized execution model. Reset of active work remains unsupported. Private GT4 startup
now passes a split GIF image transfer, StartThread and SleepThread. A missing
CD/DVD RPC endpoint and unmeasured performance reads remain startup blockers.

VCALLMS and VCALLMSR now lower to explicit native VU0 launch calls. Immediate
entries and CMSAR0-selected entries use the VU0 address mask; CMSAR0 is shared
with native EE control transfers. Launch completion retires the EE operation.
Missing bindings/entries and active ownership preserve the EE instruction and
delay-slot continuation for a retry. Macro launches preserve existing VIF inputs
without performing VIF buffer-selection transitions. No runtime instruction
decoder is linked by this path.

## Direct EE access to native vector data

LLVM memory lowering recognizes the VU0 and VU1 data windows after normal kernel
alias normalization. VU0's four 4 KiB mirrors select the same owner buffer; VU1
uses its 16 KiB buffer. The existing scalar, merged, floating and vector access
lowering operates on the selected pointer, with the existing alignment rules.
Owner extents and active VU0 work are checked before memory accesses. A missing
owner reports unsupported vector behavior. Instruction windows remain outside
these mappings and cannot become runtime executable-code storage.

The direct buffers belong to the same launch owners used by VIF uploads and
compiled VU execution. This is coherent native data sharing, not a separate
hardware-memory copy or an instruction executor. Concurrent host access is not
supported; the existing execution model remains cooperative and serialized.

## Atomic native graphics batches

The launch owner can continue across compiled graphics-request boundaries while
staging immutable packet snapshots. The same native-block budget covers every
continuation. Only complete execution submits the ordered batch to a native
consumer; rejected delivery, malformed packets or incomplete execution leave
owner state unchanged. Staging is limited to 65,536 quadwords per launch.

The batch consumer contract requires all-or-nothing external acceptance. This
permits a renderer adapter to validate a complete command plan before effects.
The production probe uses the native packet adapter described below. A
graphics request without a consumer remains an explicit unsupported stop.
Serialization and snapshots do not establish asynchronous console-transfer timing.

## Native solid-sprite packet adapter

`NativeDrawing` plans supported GIF packets into existing Vulkan sprite commands.
It accepts packed PRIM/RGBAQ/XYZ2/XYZ3, address/data configuration, and register-list
forms. Typed drawing contexts retain surface, clipping, offset and supported test
configuration; unsupported registers and rasterization features fail explicitly.
The production probe connects this adapter to compiled VU1 graphics batches.

Every packet is validated before GPU calls. Rendering uses a candidate pixel
buffer and publishes drawing state and pixels only after success. Packet size,
framing and end tags are bounded and validated. Sprite coverage planning is shared
with the existing diagnostic renderer, avoiding duplicate clipping rules. No
hardware machine, CPU interpreter or VU decoder is linked into this native path.

The implemented rasterization is untextured solid sprites with supported clipping,
frame masks and alpha-bit handling. Texture and image-upload support are described below. Other primitives,
unsupported depth tests and window presentation remain unfinished. GPU tests
establish synthetic compiled-VU rendering, not a GT4 frame or timing equivalence.

## Native direct graphics submissions

VIF1 DIRECT and DIRECTHL payloads are framed into the same immutable graphics
batches as VU output. Direct GIF submissions use the same framing and consumer.
Packet extent, end tags and total batch size are checked before acceptance; a
paused GIF owner rejects work. Configuration changes commit only after accepted
drawing. Mixed program uploads/vector-data/launches and direct graphics in one
submission remain unsupported, as do packets split across submission boundaries.

The execution model is serialized, with no outstanding competing graphics path;
DIRECTHL therefore uses the same ordered native consumer as DIRECT. This does not
implement console path arbitration, stalls or transfer timing. The adapter supports the sprite and image-upload subsets documented here.

## Native image uploads

The graphics plan now includes ordered image writes alongside Vulkan draw
commands. Host-to-local RGBA32 and RGB24 uploads support the public destination
layout and forward traversal. RGB24 retains destination alpha and carries partial
pixels across image packets. Upload configuration and progress are transactional
with drawing state; a rejected continuation can be retried without losing bytes.
The address helper is shared with the existing diagnostic graphics path.

All operations are validated before the candidate pixel buffer is updated.
Upload writes are data conversion, while sprite rasterization remains Vulkan.
Unsupported pixel formats, reverse traversal, other transfer directions, empty
surfaces and data beyond an active upload stop explicitly. Final-quadword padding
is consumed without writing extra pixels. Textured draw planning is described below; presentation
through this native adapter remains unfinished.

## Native textured sprites

The adapter now tracks UV endpoints and supported texture, filter, clamp and
alpha configuration for fixed-UV sprites. RGBA32/RGB24 surfaces use the existing
Vulkan sampler, including its supported nearest/bilinear, modulation/decal and
clamp modes. Texture setup is factored into a shared planner used by the existing
diagnostic drawing path; native packets do not duplicate sampling logic.

Perspective coordinates remain unsupported; indexed palette support is described below.
Unsupported modes fail before publishing drawing or pixels. Texture flush is a
coherent-storage boundary, with no stale native texture cache to invalidate.
The synthetic upload-to-sampling test does not establish game rendering.

## Native indexed sprite palettes

PSMT4 sprites can use sixteen-entry RGBA32/RGB24 palettes. A shared planner
converts texture configuration into bounded palette reads, including CSM1/CSM2
layout, bank selection and conditional load bases. The diagnostic path reuses
this planner; the native path has no dependency on the diagnostic GS model.

Palette reads are ordered operations in the same transaction as image uploads
and Vulkan draws. Draws consume the palette snapshot at their position in that
sequence. The final palette and remembered load bases publish only after the
whole batch succeeds. Unsupported palette formats stop explicitly. Other indexed texture formats and display presentation remain unfinished.
PSMT4 image uploads are supported as described below.

## Offline performance measurement dependency inspection

`ps2-inspect --metrics` scans executable ELF segments for performance transfers
and follows counter-read register dependencies through a bounded straight-line
window. It reports stored values, measurement-dependent memory addresses, system
writes, and unresolved flow/operation boundaries. A separate straight-line constant pass identifies candidate store addresses,
including constants formed before a counter read. It lists overlapping constant-
address loads/stores elsewhere in executable segments. Constants are discarded
at direct targets, control-flow boundaries and unknown operations. The report treats executable bytes as candidates, not proven reachable
instructions, and does not follow memory aliases or authorize an optimization.

This analysis runs only in the converter tooling. It neither installs a runtime
metric provider nor changes compiled execution. Resolving stored-value consumers
and their observable effects is required before a native measurement replacement
or elimination can be justified.

The address pass does not establish reachability or unique entry into a region.
References are investigation candidates, not whole-program alias results. A lack
of listed consumers cannot prove a measurement dead: indirect entry, dynamic
addresses, copies and external users can still observe stored data. All executable
segment extents are validated before dependency following begins.

The metric report also lists constant pointer handoff candidates matching a
measurement store's exact address or its base register value. It records stored
pointers, return values, and call arguments after applying the delay slot. Direct
call targets are reported; constants do not survive a call. This does not infer
object extents or prove that a callee accesses a particular field. Different
interior pointers and dynamically formed aliases remain unresolved.

## Native four-bit indexed image uploads

PSMT4 host uploads convert packed low-nibble-first indices into bounded masked
writes in native texture storage. The destination layout matches the existing
Vulkan indexed sampler. Each write preserves the other seven indices in its
32-bit word. Coordinate wrapping and storage wrapping follow that data layout;
no executable instructions are involved.

Upload progress survives packet boundaries and publishes with the enclosing
transaction. An odd final index consumes only its own nibble; remaining transport
padding does not modify texture data. Existing forward traversal, dimension and
batch bounds still apply. Unsupported indexed formats and reverse traversal stop.

## Native sprite pixel tests and blending

The native Vulkan sprite path supports the eight alpha comparisons, framebuffer
failure actions, destination-alpha tests and `(A-B)*C/128+D` RGB blending. Blend
factors select source/destination alpha or FIX; channel arithmetic preserves
negative floor rounding and supports clamp or eight-bit wrap. PABE can select
blending by source alpha. FBA applies after testing and blending; source alpha
is retained unless the failure action or framebuffer format masks it.

Depth writes remain unsupported. With the supported no-depth-write configuration,
alpha-failure KEEP/ZB_ONLY suppress colour output, FB_ONLY writes colour, and
RGB_ONLY preserves destination alpha. Destination-alpha testing is ignored for
24-bit framebuffers, whose destination blend alpha is 128. Invalid blend selectors
stop. Destination-dependent draws require non-aliased framebuffer coverage so
parallel shader invocations cannot race on the same destination pixel. Whole
submission publication remains transactional.

These are supported pixel semantics, not a claim of complete GS rasterization
or game rendering. Triangles, perspective textures and native window presentation
through the converted-program path remain unfinished.

## Native drawing reset lifecycle

The graphics lifecycle has an optional typed drawing-reset callback, invoked only
after successful GPU synchronization. Rejection preserves the lifecycle state and
leaves the compiled EE request retryable. The production adapter clears native
command configuration, partial vertices, palette snapshots and upload progress;
image storage is retained. CSR flush synchronizes without resetting drawing.
A reset does not execute or decode guest instructions.

## Native scalar and paired vector uploads

The data-conversion helper now accepts scalar and V2 shapes at 32-, 16- and
8-bit component widths. Scalar values populate all four lanes; V2 repeats its
first pair into the upper pair. Narrow inputs use the existing explicit signed
or unsigned widening. Component count is typed submission metadata, not a runtime
instruction to interpret. VIF framing consumes source bytes separately from the
four-byte transport padding, with existing bounded counts and destination wrap.

The family retains the existing normal-mode, equal-cycle and unmasked restrictions.
V3's fourth-lane transport behavior and masked writes remain explicit unsupported
paths. Invalid/truncated submissions publish neither data nor configuration.

## Native masked vector uploads

Supported scalar, V2 and V4 formats also accept masked uploads. Each typed data
view captures its selector word, row/column values and equal-cycle length when
parsed. The reusable converter selects input data, a row lane, a column value or
the existing destination lane. Mask groups restart at the cycle boundary and
saturate at group three within a longer cycle. Packed-colour input uses the same
selection after expansion.

All validation precedes destination mutation; the launch owner's candidate state
keeps data and packet configuration transactional. Normal row mode and equal
cycle lengths remain required for ordinary formats. V3 and nontrivial row/cycle
transformations remain unsupported. This supersedes earlier unmasked-only scope.

## Native skipped-cycle uploads and fixed row addition

Supported uploads can space groups of destination vectors with untouched gaps
when the write count is no greater than the destination-cycle length. Typed
spacing metadata determines the destination index, including storage wraparound;
no transfer engine or instruction loop is introduced. The zero/zero default
retains the existing contiguous 256-vector cycle behavior. Filling cycles and
other unequal zero encodings remain unsupported.

MODE 1 adds the captured row value to each input lane using unsigned 32-bit
arithmetic. Masked row/column replacements and preserved destination lanes do
not receive this addition. Row configuration itself stays unchanged, and packed
V4-5 retains its mode-independent expansion. Mutable-row modes remain rejected.
These capabilities supersede the earlier normal-mode/equal-cycle restrictions.

## Prepared native vector writes and mutable row modes

Upload conversion now prepares bounded typed writes containing destination,
values and lane-enable masks. The vector owner applies those writes to its
candidate storage without decoding the payload again. The planner also returns
final row values, allowing subsequent uploads and configuration commands in the
same submission to observe earlier row transformations. Plan storage remains
owned by the submission until the compiled-owner callback returns.

MODE 2 accumulates input into row values and writes the sums. MODE 3 writes input
and replaces the corresponding row lanes. Only input-selected lanes update rows;
row/column replacement and write protection leave rows unchanged. Packed V4-5
continues to ignore MODE. Configuration and vector state publish only after
successful acceptance, including any final compiled launch. This supersedes the
earlier mutable-row restriction; filling cycles and V3 still stop explicitly.

## Mask-selected filling cycles

When a positive write-cycle length exceeds the positive input-cycle length, the
planner now separates the output-vector count from consumed source elements.
Input-phase writes decode source data normally; fill-phase lanes must select row,
column or preserved destination data through the mask. Updated row values from
prior input-phase writes are visible to subsequent fill selections. Destinations
remain contiguous and wrap within their owner storage.

Input-selected fill lanes stop explicitly because their lookahead behavior beyond
nominal source consumption is not modeled. Unequal zero cycle encodings remain
unsupported. No repeating-last-value approximation is used. Payload padding and
following commands are consumed according to the input count, and malformed or
unsupported batches publish neither configuration nor vector data.

## Mask-defined V3 fourth lanes

V3-32, V3-16 and V3-8 uploads are supported when every fourth lane selects row,
column or preserved destination data. Three source components are consumed per
input vector, with ordinary four-byte transport padding. Narrow components use
the shared signed/unsigned widening and the same mask, cycle and row planner.

Input-selected fourth lanes remain unsupported: the planner does not invent a
fourth component or read past the declared source extent. If a later mask group
requires such a lane, the whole submission is rejected before publishing writes
or row configuration. This supersedes the earlier blanket V3 restriction.

## Generated executable dependency evidence

New EE projects include `cmake/native_output.cmake`. MSVC emits an executable
linker map, then `check_native_output.cmake` inspects that map and the executable's
DLL dependencies using the toolchain's dumpbin. The gate requires the generated
EE entry/object and rejects known compiler/diagnostic execution libraries,
decoder symbols, emulator dependencies and LLVM JIT runtime imports. Both MSVC
library-name formats (with and without a suffix) are checked.

Successful reports retain hashes of the executable and map plus the DLL listing.
A failed audit removes any previous success report. Maps can contain private
commercial-program metadata and must remain with ignored conversion output.
This is a known-dependency regression gate: it cannot prove arbitrary renamed or
newly introduced code lacks an interpreter. Source review, explicit native-link
composition and execution of generated code remain separate acceptance evidence.
Existing generated projects need regeneration to acquire the post-link gate.


### Native VU continuation submissions

MSCNT resumes the continuation recorded by a completed compiled VU launch.
Ordinary E-bit stops now publish the pair following the delay pair, masked to
that vector unit's address range. A new continuation submission clears a prior
completed graphics boundary; internal graphics resumes retain that boundary to
avoid executing past an E-bit stop within the same submission.

MSCNT shares MSCAL's TOP/ITOP and double-buffer updates. Data uploads, row changes,
vector state and graphics acceptance commit together. A continuation before any
successful launch, an absent compiled entry, or a rejected graphics consumer
stops without publishing the candidate state. Each submission retains the native
block budget. Continuation entries must be included in offline compilation;
missing entries never trigger runtime decoding or JIT compilation.
The converter accepts an explicit `--continuations depth` after the entry list.
Starting from all supplied roots, each round discovers the reachable E-stop
continuations and adds them as native compilation roots, up to the requested
depth (maximum 2048). Stable root sets stop expansion early. Indirect targets
still need explicit roots. Missing code, unsupported delay combinations or
unsupported operations reject generation without writing a partial module.
The default depth is zero: unrelated bytes following an E stop are not presumed
to be another program. Only padding
may follow a launch within the current atomic packet submission. MSCALF and
asynchronous execution remain unsupported.

This is verified with original LLVM-compiled synthetic programs, including an
E-bit graphics stop followed by a second continuation. It does not establish
GT4 booting or resolve the missing native performance-counter provider.


### Measurement pointer callee inspection

The offline metric report follows candidate measurement-structure arguments into
one direct callee. A bounded worklist carries candidate addresses through pointer
arithmetic and direct branches, including ordinary and annulled delay slots.
Different pointer values at a join remain separate candidates. The same address
propagation helper serves the original linear scan and callee inspection.

Reports retain unknown operations, missing code, nested calls, indirect transfers,
and exhausted work budgets as explicit boundaries. Return delay slots are inspected
before reporting the return boundary. Loads discard the destination's candidate
value; they do not read assumed runtime contents from the original ELF. This is
candidate discovery, not an alias analysis, reachability proof, or authorization
to delete measurements. Dynamic addresses and pointer escapes still require work.
The analysis itself does not change executable behavior or supply measurements.


Metric callee inspection also reports unresolved memory addresses explicitly.
Conditional moves retain an address candidate only when both outcomes share it;
otherwise later accesses are marked unresolved. Floating/vector memory transfers
have typed byte footprints, and integer quadword transfers retain their alignment
behavior. Explicit break instructions terminate inspection as flow boundaries.
The CLI accepts `--metrics [window]` with a validated bound from 1 through 4096,
allowing larger investigations without removing the work limit. These changes do
not turn candidate inspection into a proof that measurements can be discarded.


### Native profiling-source capability probe

`anyps2-host-metrics` queries Windows profiling-source descriptors through the
separate `nt_host_metrics` library. It reports source IDs, names and sampling
interval limits without changing collection settings. Source IDs are machine
capabilities, not portable constants or a mapping to PS2 event IDs. The library
bounds response allocation/retries and validates variable-length records before
publishing typed descriptors. It is not linked into generated game executables.

Windows advertising a source does not establish synchronous per-thread access,
permissions, event semantics or measurement equivalence. The documented Windows
thread-profiling API requires driver-configured hardware counters; unconfigured
slots can return zero. AnyPS2 does not treat those zeros as valid samples, install
a driver, or substitute sampled event counts for requested counter reads.
A usable measurement provider and its semantic conversion remain unfinished.


Metric pointer matching now includes bounded syntactic ancestors of structure
addresses formed by immediate addition or OR. This finds calls receiving a parent
structure rather than the exact counter-field base. The graph combines candidate
values across scanned regions and can produce false positives; it must not be
used as an alias or measurement-removal proof. Expansion stops after 128 distinct
values and reports an incomplete search. Stored-pointer candidates include their
destination when it can be resolved; absent destinations remain unresolved.


### Native counter interval attribution

The host metrics library can attribute typed, timestamp-ordered context-switch
counter samples to one native thread. Each switch-in records a processor-local
baseline; the matching switch-out contributes the difference. Migration starts a
new baseline on the destination processor. The initial partial interval is
excluded, and snapshots identify whether a full interval is available or the
thread is still running. End timestamps identify the coverage boundary.

Missing counters, reported lost events, inconsistent thread transitions,
out-of-order samples, decreasing readings and arithmetic overflow permanently
invalidate that measurement stream. No guessed wrap width, active-interval
extrapolation or zero substitute is used. The caller must serialize input, retain
source configuration identity, order events across processors and propagate
trace loss notifications. Thread identity must remain valid for the session;
a thread ID must not be reused within an accumulator's lifetime.

Synthetic interval tests do not validate ETW event delivery or establish a usable
provider. Live collection remains unverified because the isolated session probe
was denied by Windows. The elevated probe result is pending; this component is
not connected to guest performance requests or shipped game output.


The host library now decodes Windows Thread_V2 context-switch records into owned
counter samples. It checks the provider, opcode, version, payload extent, timestamp
and a single PMC extension containing exactly the configured number of readings.
Other event types are ignored; missing, duplicated or mismatched counter data is
rejected. CPU indices use the SDK's processor-index accessor. Returned samples
own their counter data and can outlive the callback buffer.

This adapter accepts live Windows callback records, not arbitrary serialized
pointers. A capture owner must catch decoding failures and invalidate the stream,
track event loss and source identities, and ensure ordering before attribution.
The event adapter and attribution pipeline have synthetic coverage. No live
capture or guest measurement provider is claimed yet.

### Private startup comparison

The generated probe accepts `--capture-stop <new-private-directory>` to record
CPU registers, the stopping PC and reason, RAM, and scratchpad after execution
stops. Use an ignored directory under `work/` for commercial inputs. Existing
directories are rejected before execution. `capture.txt` is written last and
marks a completed observation; a failed write can leave an incomplete directory.
These files can contain proprietary code and must never be committed.

This is a diagnostic observation, not a resumable save state: native service,
thread, Vulkan and VU state are not serialized. Emulator observations can guide
startup comparisons but do not establish native correctness, authorize metric
removal, or become dependencies of the generated executable.

### Unobserved performance-counter configuration

Without a metric provider, counter-control writes and reads are configuration
operations. Potentially active counters are marked unavailable instead of
accumulating synthetic values. Disabling counting preserves that mark; an
explicit write while disabled restores a known value for that counter. A read
of an unavailable counter stops before retiring the read or changing its output
register. No cycles, cache misses or overflow interrupts are synthesized. This
supports startup configuration, not measured profiling or full performance-unit
exception semantics. Installed providers retain their existing atomic contract.

Object-table entry discovery now recognizes bounded constant-offset table loads
feeding indirect calls, paired with recoverable table pointers stored into the
same object-field offset. It adds executable ELF targets as compilation candidates;
it does not assume writable tables remain constant or replace runtime dispatch.
Clobbered/dynamic bases and invalid targets do not produce entries.

The discovery pass also proposes entries from bounded indexed switch tables:
a matching unsigned range check, scaled index, table-base construction and
indirect load/jump must be present. Table extents are capped and every entry
must resolve to executable input bytes. Runtime guards, reads and destinations
remain unchanged; candidate recovery is not constant folding.

### Split native graphics transfers

The GIF transport accepts aligned fragments of a bounded packet across separate
submissions. It retains incomplete tag/payload data and submits complete EOP
packets to the existing transactional native drawing consumer. No pixels or
configuration from the incomplete packet are published early. Consumer rejection
preserves the prior framing state so the completing fragment can be retried.
GIF reset discards pending framing. Pending packet/status-register queries and
VIF1 interleaving are rejected until that path is complete; cross-path arbitration
and hardware queue/timing status are not modeled. Each combined pending/input
batch is bounded to 65,536 quadwords. This frames graphics data, not instructions.

SetupThread's registered return trampoline is now an offline compilation root
when its argument can be recovered from executable input. StartThread diagnoses
the actual missing entry or return trampoline, with no runtime compilation.

### Native sleep and wakeup scheduling

SleepThread consumes one queued wakeup or suspends the current continuation and
selects a ready thread. WakeupThread releases a sleeping continuation or queues
one wakeup for a ready thread; dormant, invalid and currently running targets
are rejected. Cancellation returns and clears the queued count. Status queries
report the sleep wait reason and queued wakeups. Starting a dormant thread clears
old wakeups. Counter overflow is rejected without mutation.

The interrupt-safe wake/cancel aliases update scheduler state without switching
inside a callback. A pending higher-priority continuation can be selected after
the interrupt chain restores its interrupted context. Ordinary wakeups may
preempt at the native service boundary. No host threads or PS2 instruction
execution are created: all continuations execute compiled LLVM code.

An explicit native idle-pump owner permits sleep with no ready replacement. The
event loop pumps external completions and display events, dispatches compiled
interrupt handlers, and resumes only after a real wakeup. Idle polls do not retire
guest operations. A bounded wait returns a resumable `thread_wait` diagnostic;
without an owner, sleep still stops before changing the thread state.


### Counter selector handling and reference limitations

The explicit per-counter disable selector (16), and counter 0's unused selector
0, preserve the last written value even when global/mode enables are set. A
selector change does not erase an earlier unmeasured interval. An explicit write
while that counter is disabled restores a known value. Reserved selectors remain
unsupported when enabled; they are not treated as measured zero events.

Native stop diagnostics report the failing operation (0 read control, 1 write
control, 2 read counter, 3 write counter) and counter index. This distinguishes
configuration acceptance from the first unresolved measurement.

The pinned PCSX2 reference accumulates its virtual execution cycles for event 1
and does not accumulate instruction/data-cache misses for event 6. These are
reference compatibility choices, not host performance measurements or evidence
that profiling can be removed. The current native integration still stops on an
active counter-0 cycle read. Windows ETW collection alone cannot establish the
required guest-to-native meaning. Replacing or eliminating the measurements
requires resolving their consumers, including memory aliases and loaded code;
the existing candidate report does not prove those uses are unobservable.


Metric pointer exploration follows bounded nested direct calls and constant-target
indirect calls. Link registers are assigned before call delay slots; branch and
return targets are captured before their slots. A matching return resumes with
the callee's resulting candidate registers rather than assuming ABI preservation.
Unresolved targets, unknown operations and conditional-link branches remain
explicit boundaries. Call depth is capped at 32 in addition to the instruction
exploration budget. This remains candidate discovery, not a whole-program alias
analysis or permission to remove measurements. Scalar and packed bitwise/copy
operations propagate their known low-word candidates. Floating-point and vector
arithmetic preserve integer candidates, while transfers back into integer
registers invalidate their destinations. Copies from untracked upper lanes stay
unknown, and vector code launches remain boundaries.


### Native CD/DVD initialization RPC

With a configured disc, endpoint `0x80000592`, method 0 owns a native ISO session.
Mode 0 opens the ISO and validates its root directory, mode 1 opens the volume,
and mode 5 releases the session. Reinitialization replaces ownership only after
validation succeeds. Unsupported methods/modes stop explicitly. Four-byte legacy
and 16-byte extended responses contain success and zero legacy version/logging
metadata, without claiming Sony module identities.

Response bounds and executable-write protection are shared with FILEIO and are
checked before any lifecycle change. Media failures preserve the previous session
and response bytes and expose a diagnostic. Other CD/DVD RPC endpoints and physical
media control are not implemented by this endpoint. Passing initialization is
startup progress, not visible boot. Active counter measurements remain unresolved
independently.

Synchronous command endpoint `0x80000593` shares that session. Method 1 returns
the native clock in the public BCD layout. Method 34 stores media mode 1 (CD) or
2 (DVD) and returns success. The mode remains available to later sector reads and
is cleared when initialization shuts the session down. Other command numbers and
media values stop without writing a result.

Disk-ready endpoints `0x8000059A` and `0x8000059C` share the session. An open
ISO with no transfer returns command-complete for the blocking and polling
modes, and status byte `0x40` for the status query. A closed session polls as
not ready. The blocking mode does not wait, because there is no transfer to
finish.

## Explicit native thread-cycle profiling

The native executable accepts `--native-thread-cycles` to opt into host CPU-cycle
measurements through Windows `QueryThreadCycleTime`. This replaces all-mode cycle
profiling with measured CPU work on the native owner thread, including native
services and Windows kernel work on that thread. It is not an R5900 cycle model,
wall-clock time, or a conversion to PS2 frequency. It can affect applications
which interpret profiling values as console time; no such equivalence is claimed.
Use `--strict-performance` to stop on unmeasured active reads instead of selecting a native provider.

The provider supports event 1 only when all four guest mode enables are set.
Mode-selective counters and other events remain unavailable: the mode at a read
cannot attribute an earlier interval. Unsupported intervals remain unavailable
across disable/reconfigure operations until an explicit counter write resets
that history. Disabled counters retain their written or measured values. Counter
addition wraps at the guest 32-bit width. Missing or regressed samples stop without
retiring the transfer or committing counter state. Attaching to an already active
counter does not recover measurements from before attachment.

The real GT4 integration passes the first cycle read with this policy, then stops
at the following instruction-cache-miss read. No cache values are substituted.
This is three additional retired EE operations, not a frame or a completed wakeup.
The native dependency audit still passes. Compiled synthetic transfer tests cover
wraparound, unavailable events, disable/reset, failed/regressed acquisition,
mode restrictions and late attachment. ETW source enumeration alone is not proof of a valid measurement.

`--native-pmu` selects a separate sampled native profiling policy backed by a
private Windows ETW system session. It configures the advertised `TotalCycles`
and `IcacheMisses` sources and attributes their deltas only to completed on-CPU
intervals of the native execution thread. It requires Windows system-tracing
permission. No driver is installed and no other trace session is modified.

Samples are asynchronous: reads expose the latest delivered completed intervals,
including native service work, and resets start a new delta from that observed
snapshot. They are not synchronous PS2 performance-register measurements. The
active interval is never extrapolated; the first sample waits for a real interval,
then reads do not block the display handler awaiting a new context switch. This
avoids profiling itself creating a permanent display-interrupt backlog. Lost or
malformed events, discontinuities, counter regression and failed capture stop
execution. A missing measurement never becomes a successful zero reading.

Windows can emit scheduling records with equal incoming/outgoing thread IDs.
The accumulator closes and reopens that measured interval, preserving its actual
delta without double-counting. Synthetic tests cover this and reading a completed
prefix while another interval is active. A live collection probe has now measured
both sources with no reported loss; this establishes host measurement availability,
not PS2 timing equivalence or a game boot.

## Native elapsed-time profiling

The generated Windows executable defaults to `QueryPerformanceCounter`. No
administrator access, ETW session or hardware-counter configuration is required.
The native service replaces all-mode cycle profiling (event 1) and the secondary
instruction-cache profiling slot (event 6) with elapsed clock ticks. Both slots
use the same monotonic clock with independent reset values. This is an explicit
change of profiling semantics, not a PS2 cycle/cache model. It does not establish
that game calculations based on the original units are equivalent.

The runtime reports `QueryPerformanceFrequency` and identifies the units as
elapsed ticks, including time spent waiting and in native services. No synthetic
rate, PS2-frequency conversion or cache-miss estimate is used. Disabled counters
retain their values; writes reset the sampled baseline; addition wraps at 32 bits.
Unavailable or regressed samples stop without committing the transfer. Unknown
selectors and mode-selective measurements still stop. `--strict-performance`,
`--native-thread-cycles` and `--native-pmu` are mutually exclusive diagnostic
alternatives to the default clock.

Compiled synthetic counter transfers cover both clock-backed slots, wrapping,
reset/disable behavior and failed/regressed acquisition. The actual GT4 native
executable passes both reads without elevation and reaches graphics initialization;
this is startup progress, not a rendered frame.
## First GT4 render-target milestone

The native LLVM executable now submits GT4's own packets through the Vulkan
sprite path and produces the opening copyright/driving-warning image. Private
readback inspection confirms game-generated text, not a cleared target or test
pattern. The configured 640 x 448 display surface now contains that same image
and is presented in the diagnostic Vulkan window. Continuous presentation,
the title screen, native input and gameplay are not established.

GS initialization supports scanline parity masking in the shader, TEX2 masked
texture updates, mip-level base configuration, suppressed vertex kicks and flat
sprite color with IIP set. Unsupported mip sampling, fogged drawing, dithered
16-bit targets and dual-circuit display blending remain diagnostic stops.
Descriptor-only GIF submissions complete without inventing drawing work.

The native serial adapter supports compiled byte transfers, bounded buffered
transactions and completion interrupts for explicitly disconnected ports. It
reports missing devices, fills response bytes from the unacknowledged line state,
and publishes receive buffers before completion. It does not provide a working
controller or memory card. Slice descriptors become native buffer operations;
there is no DMA timing loop or runtime instruction decoding.

`--capture-stop` keeps raw graphics storage and a last-render-target PPM under
the requested private capture directory. That image is bounded by the last draw,
not necessarily the display extent. A separate selected-display PPM captures the
game-selected surface when a display circuit is enabled. By default the executable
presents that 24/32-bit surface in a diagnostic window at its configured resolution;
closing it ends inspection. Execution is stopped throughout this diagnostic view.
`--headless` disables the window for automated tests; `--display-stop` explicitly
reenables it.

EE semaphore waits now suspend native continuations, expose their waiter count
and transfer signals directly to FIFO waiters. Interrupt-safe signals defer
scheduling until interrupt return. The persistent IOP session continues after
completed worker threads while module startup still observes individual entry
returns. Word-aligned IOP transfers and extended EE callback/switch discovery
advance GT4 past the former wait, transfer and RPC-dispatch boundaries. The
current run stops at a further indirect EE target absent from the AOT code set.

## Static native layout conversion

Conversion recovers bounded initial-data layout candidates at recognized public
layout-service call sites. These are proposals, not a proof that writable input
data is immutable. Conflicting backing or access permissions reject conversion.
`native-layout-candidates.txt` records proposed records; `native-layout-plan.txt`
records accepted relocations and restrictions in the private output directory.

Supported extra RAM views become constant range comparisons and pointer-offset
arithmetic in LLVM loads and stores. They share the original native allocation;
there is no runtime mapping array, lookup loop, instruction interpreter, or JIT.
The native allocation plan exists from process startup. This intentionally changes
allocation availability: mapping declarations validate the fixed plan rather than
create or evict mappings. Programs depending on dynamic remapping, address-space
readback, or pre-initialization access faults are outside this conversion contract.

Complete layout transactions compare all incoming declaration fields against the
compiled plan and validate backing extents. Unrecognized mappings stop. Clearing
an unallocated high-address range is permitted only when generated checks prove
it is disjoint from compiled aliases; clearing a live allocation is unsupported.
Allocation reservation requests within the plan require no eviction state, and
reservation readback remains unsupported. Native device identity bindings retain
compiled read/write restrictions. Binding a device address does not create BIOS
storage, implement an unsupported operation, or report device readiness.

Native C++ services still validate their own pointer domains. An extra alias is
not automatically accepted by every service merely because generated loads and
stores support it; service-boundary conversion must preserve the same backing
before such pointers can be accepted there.

Private GT4 validation completes the populated layout declarations and vacant
entry cleanup, then reaches ExecPS2. The loaded secondary image has not yet been
linked into the boot executable's AOT code set. The warning still renders before
the black handoff screen; a title screen is not established.
