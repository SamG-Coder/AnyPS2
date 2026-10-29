# EE LLVM native conversion

### Initial EE LLVM regions

`nt_ee_codegen` reuses the R5900 decoder and accepts a sorted instruction set of up to
262,144 validated decoded instructions. Each generated EE function contains at most
512 instructions. It emits LLVM integer operations for LUI,
ADDIU/DADDIU, ADDU/SUBU/DADDU/DSUBU, immediate shifts, and scalar bitwise operations.
Word results sign-extend to 64 bits; scalar writes preserve each register's upper
64 bits. Register zero reads as zero and ignores writes. Arithmetic wraps without
LLVM overflow promises. The emitter validates the entire region before writing
output and rejects unsupported instructions and inconsistent decode metadata.

The native ABI shares the existing EE state declaration through `ee_state.hpp`,
without linking the diagnostic runtime. Generated control flow uses an entry PC
switch followed by direct compiled blocks, with an operation budget that supports
resuming at each boundary. A syscall retains its PC and does not retire; native
service handling must be connected separately. J/JAL, JR/JALR and BEQ/BNE capture
their targets before link writes or delay-slot execution. Branch retirement saves
the pending destination and origin so a budget boundary can resume the delay slot.
Delay completion selects the compiled destination through the region's PC switch;
unknown or unaligned targets stop without interpreting code. Incoming delay state
must identify the preceding compiled branch. Branches and syscalls in delay slots
are rejected at conversion time. BEQ/BNE compare the full lower 64-bit operands.
Other branch forms, device access, exceptions and most floating/vector operations remain missing;
a converted EE/IOP peer is not yet connected.


EE scalar memory lowering supports LB/LBU, LH/LHU, LW/LWU, LD and SB/SH/SW/SD.
It emits direct LLVM loads/stores into a caller-owned RAM span. Effective addresses
wrap to 32 bits; kernel cached/uncached aliases resolve to physical RAM. Alignment,
complete access extent, null backing storage and the 32 MiB RAM limit are checked
before any access. Signed loads extend to 64 bits; upper register halves survive.
Loads into register zero still perform address checks. No load-delay model is added.
The current generated ABI requires a little-endian 64-bit host.

Stores overlapping this region's compiled instruction addresses stop with
`code_write`, including through aliases. The ELF front end passes all executable segment ranges, including
undiscovered code and zero-filled executable extents. Library callers may provide
additional physical protection ranges; emitted instructions are always protected. Faults return the effective address through the native
Memory descriptor and leave PC, retirement count and delay-slot continuation
unchanged. The fault address is diagnostic output, meaningful only for a memory
or code-write stop. Device registers, scratchpad and unaligned merge operations
remain unsupported; they are not silently mapped into RAM.


### EE ELF project generation

`ps2-recompile` parses a physical-RAM MIPS ELF and reuses direct-flow discovery with
an explicit stop-at-syscall option and recognized native continuation roots. The existing inspection mode still follows
syscall continuations. Sparse discovered instructions become bounded LLVM functions;
indirect destinations must resolve to compiled entries at execution time. This is
bounded entry-flow conversion, not proof that every loadable game function is covered.
Overlapping segments, non-RAM layouts, missing direct destinations and unsupported
reachable operations fail before the output directory is created.

The generated project retains LLVM IR and initialized data with zero-filled BSS.
Its executable links the shared native probe and binary reader, without decoder,
interpreter, diagnostic runtime or hardware models. The probe checks data size and
build fingerprint, supplies zeroed remaining RAM, and invokes compiled code from
the ELF entry. Unsupported services and peer readiness remain explicit stops. Windows
x86-64/MSVC with Clang is the initial build target. Return codes distinguish budget
stops (4), other execution boundaries (3) and input errors (1).


EE PADDUW lowers to `llvm.uadd.sat.v4i32`, updating all four 32-bit lanes of the
128-bit destination. Operands are loaded before the destination write so either
source may alias the destination. Register zero supplies a zero vector regardless
of backing-state contents and discards writes. LLVM owns target-specific SIMD
selection; no measured performance claim follows from using the intrinsic.
MTHI/MTLO/MFHI/MFLO and their secondary-bank forms share accumulator-transfer
lowering. They transfer the lower 64 bits, preserve destination register upper
halves, and leave the other accumulator bank unchanged.


MTSAB and MTSAH update the native shift-amount state from masked source/immediate
bits using byte units; MFSA transfers that value to the lower 64 bits of its
integer destination. MTSAB uses four low bits; MTSAH uses three and doubles the
result. The immediate is masked before use, including negative immediates.
Direct MTSA and packed funnel-shift execution remain unsupported.
MTC1/MFC1 transfer the raw 32-bit floating-register representation. MFC1 sign-extends
into the lower integer half and preserves its upper half. These transfers do not
perform arithmetic or alter FCR31; NaN-like and signed-zero bit patterns survive.


ADDA.S, ADD.S and SUB.S call a small native arithmetic primitive with a fixed-width
C ABI. The primitive shares the existing integer-based finite add/subtract routine
with diagnostic consumers through `ee_float.hpp`; no interpreter or device model
is linked. It flushes denormal operands to signed zero, chops finite results,
preserves signed-zero behavior and reports underflow independently of host rounding
mode. Generated code writes the accumulator or destination and updates current
underflow/overflow flags while retaining sticky and unrelated flags. Exponent-255
operands and extended-range results return `unsupported_numeric` before changing
destination, flags, PC or retirement state. These cases remain unfinished.

SYNC modes 0 and 16 lower to an LLVM sequentially consistent fence. This conservatively
orders native accesses without modeling a console cache or instruction pipeline.
Reserved modes/fields reject conversion. No DMA/peer completion is implied by the
fence; those still require explicit native services.


CTC1/CFC1 currently support FCR31. Writes update only condition/status bits
(mask 0x0083c078), preserving other bits; reads sign-extend the stored 32-bit value
and preserve the integer destination's upper half. Other selectors reject
conversion, including implementation/revision queries; no processor identity is
fabricated. The control-register write does not change host floating-point mode.

LQ/SQ reuse scalar memory validation and code-write protection, rounding the
effective address down to a 16-byte boundary as required by their public contract.
The whole 16-byte extent is validated before a vector load/store. Both register
halves transfer together; a load may overwrite its address register only after
capturing the address. Zero-register loads still validate memory and stores write
a zero vector. Fault diagnostics retain the original effective address.


SetupThread is the first connected native EE syscall. The startup service owns the
main-thread descriptor and reuses a shared initial-stack planner with diagnostic
consumers. It validates stack extent/alignment and the complete argument buffer
before publishing metadata or writing the explicit empty launch context. Native
argument writes use the same generated executable-range predicate as compiled
stores. On success it returns the stack pointer, retires the syscall exactly once,
and resumes compiled code within the original budget. Invalid memory, code writes,
unsupported calls and repeated setup leave the boundary unretired.

The ELF front end discovers continuation candidates for syscall sites immediately
preceded by a constant assignment of a supported startup service number to register 3. This
pattern is generic and does not select behavior by title/address. It is only a
compilation candidate: native dispatch checks the actual syscall and arguments.
Other call-number constructions remain discovery gaps. Main-thread metadata does
not yet provide a scheduler or an EE/IOP connection.


SetupHeap and EndOfHeap share a bounded heap extent owned by the native startup
service. A fixed size must not wrap or cross the main stack base; size -1 extends
to that base. Empty heaps are valid. EndOfHeap reads the last accepted extent;
neither call creates an allocator, writes heap memory or claims allocation success.
Both require the relevant prior setup state. Rejected changes preserve the previous
heap and leave the syscall unretired. Successful metadata updates and queries use
the same return/budget path as SetupThread. A shared syscall registry controls
which constant-number continuation candidates the converter follows.

Heap extent planning is shared with diagnostic kernel consumers, avoiding separate
rules. Discovery has a separate 262,144-instruction program bound. The compiler partitions
that set into functions of at most 512 instructions, keeping each branch and delay
slot together. LLVM noinline attributes retain these function boundaries.


A generated balanced range dispatcher selects compiled functions by PC. Cross-part
transfers return to an iterative dispatcher with the remaining operation budget;
there is no recursive host call chain or runtime instruction decoding. A destination
inside the current part that failed its exact PC switch remains an unmapped-code
stop, preventing repeated dispatch without progress. Holes and unaligned targets
stay explicit errors. Branch state survives pauses between branch and delay slot.
Executable protection ranges are sorted and merged once for the entire program,
so stores in any part cannot modify code belonging to another part.


EE LLVM interrupt controls preserve the public EI/DI privilege conditions and
modify only the EIE bit in shared status metadata. Native producers may publish
pending interrupt classes (bits 8..15) between runner calls. Generated code checks
IE, EIE, exception levels and the matching class mask at instruction boundaries;
branch delay slots finish before delivery. A ready event returns interrupt_pending
without retiring the next instruction or clearing any pending class. No guest
handler or acknowledgement is invented. This is a cooperative native boundary,
not a concurrent event queue or a cycle-accurate interrupt scheduler. Handler
routing and native event producers remain unfinished.

SLT/SLTU and SLTI/SLTIU lower to signed/unsigned LLVM 64-bit comparisons, with
sign-extended immediates in both immediate forms. Results replace only the low
64-bit destination lane; register zero and upper lanes retain their contracts.


EE conditional moves use a 64-bit zero/nonzero test and LLVM select, preserving
upper destination lanes and capturing aliased operands before the write. Integer
conditional branches include signed zero comparisons and likely forms. An untaken
likely branch skips its delay instruction entirely, including memory checks and
retirement, while a taken branch retains the existing resumable delay-slot path.
Branch condition lowering is shared in branches.hpp.

Scalar shift lowering shares one width-aware path for immediate and variable
32/64-bit operations. Variable counts are masked to five or six bits before LLVM
shifts; the 64-bit high-immediate forms add 32. Word results sign-extend to the low
64-bit lane, and upper lanes remain unchanged.


EE LWC1/SWC1 reuse the checked RAM access path, including alignment, alias
translation, extent validation and executable-write protection. Transfers preserve
all 32 bits, including signed zero and extended-range bit patterns; FPR0 is an
ordinary writable floating register. Faults leave the instruction unretired and
preserve memory and destination registers.

Floating comparisons lower to ordered LLVM comparisons after exponent-zero
operands are normalized to zero. Finite values compare exactly without host
rounding-mode dependence. Exponent-255 operands remain unsupported and stop before
changing FCR31; unconditional C.F clears the condition regardless of operands.
Only the FCR31 condition bit changes. BC1F/T and likely forms reuse the shared
branch and delay-slot lowering. No floating arithmetic or approximate extended
value conversion is introduced by these operations.


CFC2/CTC2 lower to fixed native vector-control service calls, with selectors and
wait flags resolved at conversion time. The service API is typed C++ and receives
no guest opcode stream. Read results sign-extend 32 bits while preserving the
upper register lane; writes pass the source low word. Reads targeting integer
register zero still synchronize, and writes sourced from zero pass zero.

The native owner reports ready, pending, or unsupported. Pending calls retain PC,
retirement count and delay-slot state for a retry. Pending/unsupported writes must
commit nothing. The generated probe has no vector owner yet and returns an
explicit unsupported-vector stop; no idle status, successful reset or completed
vector job is fabricated. Synthetic test owners validate the ABI and resumable
control flow only. CFC2's random-register selector remains rejected at conversion
because its special transfer semantics are not covered by this adapter.


LQC2/SQC2 access caller-owned VectorStorage shared with ahead-of-time vector code.
Missing storage stops as unsupported; storage marked active stops as pending.
Ownership is cooperative between calls, not a lock-free concurrent access promise.
Transfers use the common RAM bounds, alias and code-write guards. This milestone
accepts aligned 16-byte vector accesses only; misaligned addresses stop explicitly
rather than borrowing the integer LQ/SQ rounding rule. VF0 reads as (0,0,0,1.0)
and ignores writes while still checking the memory access. Other vectors transfer
all 128 bits unchanged. The production probe does not yet attach vector storage
or the control owner; these data paths do not establish VU execution.

MOV.S copies raw bits. ABS.S clears and NEG.S toggles the sign bit, including for
zero, denormal and exponent-255 patterns. MOV.S leaves flags unchanged; ABS.S and
NEG.S clear current overflow/underflow flags while preserving sticky and condition
bits. No host floating arithmetic is used for these operations.


MFC0/MTC0 selector 25 supports disabled performance-control metadata, counter
writes and sign-extended readback. It owns control and two counter values, but
never accumulates synthetic cycles. Attempts to enable counting or access active
measurement state stop as unsupported-system before mutation. Other selectors are limited to the status and exception-return metadata described below. MTPS encodings with a nonzero register
field preserve the public ignored-write behavior. Native measurement providers
and active performance events remain unfinished.

MULT/MULTU/DIV/DIVU lower to integer LLVM operations using low 32-bit operands.
Both HI and LO receive sign-extended word results, including unsigned operations;
multiply also writes its optional scalar destination. Division widens operands
to avoid signed overflow and substitutes a safe divisor before selecting the
specified divide-by-zero result. Secondary accumulators and upper lanes remain
unchanged. BREAK emits an explicit breakpoint stop, retaining PC and branch-delay
metadata without retirement or an invented exception handler.


MUL.S uses the shared original finite integer-significand multiplication helper.
The LLVM adapter shares result/flag handling with finite addition. Normalization,
truncation, signed zero and underflow are independent of host rounding mode;
extended operands/results stop before destination or flag mutation. The diagnostic
runtime reuses the same pure helper without being linked into generated output.

VADD/VSUB and broadcast forms call a fixed native arithmetic primitive with
conversion-time register/lane arguments. The primitive receives no instruction
stream. It uses the shared finite arithmetic and vector storage, snapshots aliased
operands, preserves unselected lanes, and commits result/MAC/status together only
when every selected lane is supported. Missing/active storage stops explicitly.
The diagnostic vector arithmetic also uses the shared pure calculation. Connecting
the production vector owner and converting vector microprograms remain unfinished.


VMOVE and VMR32 lower directly to LLVM vector selection/shuffle operations. They
reuse the same storage ownership guard as vector memory accesses. Lane masks,
aliased sources/destinations and VF0 are preserved without floating interpretation
or status-flag changes. All source lanes are captured before the destination store.

LDL/LDR use the checked RAM path to load the containing aligned eight-byte word,
then merge selected bytes into the destination's low 64-bit lane. Original fault
addresses and upper register lanes are preserved. Shift counts remain within
0..56, including boundary offsets, so LLVM never receives an out-of-range shift.
Loads targeting integer zero still validate memory. Paired loads may span aligned
words; each instruction commits independently after its own successful access.


SDL/SDR share bounded byte-merge lowering with LDL/LDR. Stores validate the full
aligned eight-byte read/modify/write extent and executable protection before
reading or changing RAM. Unselected bytes survive, source zero supplies zero,
and failed accesses retain PC and original memory. The current code-write guard
protects the full aligned word; it conservatively rejects a partial store sharing
that word with executable bytes, even if those bytes would retain their values.

System transfers now include status, EPC and ErrorEPC metadata already owned by
the native state. Writes consume the source low word; reads sign-extend and status
reads apply the public visibility mask. A status write can expose a pending event
at the next safe boundary. This does not implement exception dispatch, virtual
memory or a BIOS return path. PLZCW lowers two independent signed-word leading-bit
counts through LLVM ctlz, excluding each original sign bit and preserving upper lanes.


Signed ADD/SUB/ADDI and DADD/DSUB/DADDI use LLVM signed-overflow intrinsics.
Overflow returns arithmetic_overflow before destination writes or retirement,
even for integer register zero. PC and pending delay-slot context remain intact.
Word results sign-extend; doubleword operations use 64-bit operands. No overflow
exception handler is synthesized. Wrapping unsigned forms retain their prior path.

PCPYLD/PCPYUD/PCPYH use LLVM vector shuffles with both sources captured before
writing. Packed AND/OR/XOR/NOR operate across all 128 bits. These operations honor
integer register zero and handle source/destination aliases without scalar-only
writes or host floating interpretation.


PCEQB/PCEQH/PCEQW lower to LLVM lane equality and sign-extension of the boolean
result, producing all-one or zero lanes. PPAC5/PEXT5 use integer vector masks and
shifts for exact color-bit placement, without scaling discarded component bits.
Byte/halfword/word packing and lower/upper interleaving use one parameterized LLVM
shuffle path. Both sources are captured before writing, including aliased
operands; integer register zero retains its constant and ignored-write behavior.
