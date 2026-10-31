# Testing AnyPS2

PADMAN service regressions exercise timer ownership, elapsed-time compare events,
compiled interrupt callbacks, vblank registration/removal, thread status, IRX
callback relocations and guarded indexed registry discovery. EE tests cover
chronological display edges, bounded catch-up and idle waits that preserve the
operation count. Commercial startup evidence remains separate and opt-in.

Native serial integration fixtures execute generated LLVM through the service
router. They cover configuration, cached aliases, interrupt acknowledgment,
independent sessions, reset, rejected START/FIFO/status operations, and load and
branch delays. These checks do not establish controller transfers or game boot.

Current policy: LLVM is the only supported conversion backend. Older sections
below describe historical diagnostic C++/interpreter milestones; those are not
supported output paths. The EE C++ generator and its execution test have been removed. IOP milestones now run through iop-llvm-execution and iop-llvm-project.


## Default tests

Add tests for major behavior changes and integration milestones. Extend an
existing subsystem regression when practical; small edits and helpers do not
need separate tests that duplicate their implementation.
Run only suites affected by the current change, including relevant integration
checks. For example, an isolated image-loader change builds `format-tests` and
runs `ctest --test-dir build -C Release -R '^formats$' --output-on-failure`.
The commands below describe an explicit full validation run, not the default
requirement for each edit.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Default suites cover synthetic ISO/ELF parsing, control-flow discovery, compiled
generated instructions, kernel behavior, hardware registers, process arguments,
repository content, and rejection fixtures for the repository policy. They need
no commercial game or BIOS. `conversion-inventory` builds a synthetic disc with
one convertible IOP module, one unsupported sibling, an EE executable and an
unrecognized file. It checks that the sibling still converts, unrecognized bytes
are not decoded, and the report keeps linked and executed coverage at zero.
Names that differ only by a separator, only by letter case, and long names that
share a 60-character prefix, keep distinct artifact directories and distinct IR.
Each generated unit
records its source identity beside that IR. A generated native fixture is compiled by the normal
build so tests execute the generator's output.

The `iop-native-execution` milestone compiles generated IOP C++ without linking the
interpreter. Its RAM contains data only, proving execution does not fetch guest
instruction bytes. It checks branch/load delays, budget resumption, RAM aliases,
signed/unsigned byte and halfword access, alignment faults and rejection of writes
to converted instructions. Fixture generation also rejects unsupported code and
unresolved indirect transfers before emitting output, and checks the generated
function bound including delay slots.
The `iop-native-project` milestone converts an original synthetic IRX, configures
and compiles its generated CMake project, removes the original IRX and executes
the resulting binary. It checks the return value, rejects malformed data size,
and checks generated source/build dependencies for interpreter references. These
tests cover the standalone converter, not full IOP service or game conversion.
Run them with `ctest --test-dir build -C Release -R '^iop-native-' --output-on-failure`.

The disc-file-I/O milestone uses a synthetic ISO and exercises the actual SIF
request/reply path. No commercial image is needed for that test.
The SIF transport milestone also executes synthetic IOP loads/stores through
physical, cached and uncached SBUS aliases. It checks cross-processor command
addresses and flag acknowledgement, shared control state, zero-read F260,
rejected widths/control bits and absence of an implicit bus when disconnected.
The format milestone includes synthetic reboot archives and relocatable IOP
modules. It checks signed high/low relocation carry, jump and word relocation,
zero-filled BSS, immutable input, truncated tables, memory bounds, unmatched
relocation pairs and unsupported forms. This proves image preparation only;
guest registration calls and startup execution have separate coverage. Synthetic
IRX fixtures additionally cover module metadata, import tables, ordinal/version
requests, guest jump patching, preserved delay slots, malformed stubs and atomic
failure when a later import cannot resolve. No commercial module is a fixture.
The same milestone loads two synthetic providers, relocates their export tables,
registers their libraries, and binds a consumer to the resulting guest addresses.
It checks duplicate/malformed registration, bounded ordinals, exact ABI matching,
missing-provider rollback and registry reset. This does not execute the modules.
The `iop-execution` milestone separately executes an original synthetic consumer
through a registered import, runs the provider and returns a computed result.
It also covers branch/load delays, taken and untaken paths, delayed-load write
collisions, budget suspension, RAM aliases and precise unsupported/fault stops.
It uses no BIOS, commercial instructions or game-specific behavior.
The execution milestone also loads an original synthetic IRX and starts it with
arguments in guest RAM. It verifies stack alignment/save space, terminated argv,
BSS initialization, a return value computed by guest instructions, suspension
before the return delay slot, stable terminal results and rejection of invalid
startup inputs before caller RAM changes. Unsupported entry code faults rather
than reporting successful startup. This is not a commercial-game boot test.
The register-argument entry form is covered by a synthetic routine consuming all
four argument registers. A separate ignored local probe can load archive modules
and exercise an explicitly selected public entry ABI; its results do not establish
that the EE reset sequence or full IOP runtime is operational.
Resident-call coverage initializes a synthetic module once and invokes a function
repeatedly to update a persistent BSS word. It also rejects calls into BSS before
changing caller RAM. This guards against accidentally reinstalling the original
image between exported-function calls.
The disc-file-I/O milestone now sends a reboot request through SIF using a
synthetic ISO containing an original memory-bootstrap IRX. It verifies actual
instruction execution and retained reboot context, while reset count and BOOTEND
remain unset. The IOP execution milestone checks bootstrap budget resumption and
rejection of an archive without the required bootstrap module.
The reboot integration fixture includes a synthetic guest allocator and a manager
that imports it. Tests check guest allocation, relocation/binding, repeated
preparation, invalid/overlapping allocation results and unresolved-import
rejection before manager code is installed. Manager preparation is not startup.
The handoff fixture executes its manager entry to read the RAM-size field and
checks linked module records, archive pointers and the terminated module list.
Status-register tests cover transfer delay and rejection of unsupported modes;
fault tests verify the effective address is reported without partial retirement.
Device coverage checks physical/cached/uncached aliases, read-after-write state,
load delay, rejected widths/PS1 mode and unknown addresses. The synthetic manager
also reads ICFG through the device interface during the SIF reboot test.
DMA-control coverage verifies retained word writes and rejected partial accesses;
the synthetic manager reads that register during reboot as well. No successful
DMA transfer is inferred from the configuration-register tests.
Cache-maintenance coverage enters isolation, suppresses byte/halfword/word RAM
stores across a budget suspension, accesses kernel cache-control configuration,
then restores normal RAM writes. It checks exact-address routing, rejected
partial configuration writes and explicit rejection of isolated cache reads.
The same IOP execution suite checks signed/unsigned multiply and divide, HI/LO
transfers, negative remainders, division by zero, signed division overflow and
retained results across budget suspension.
Archive dependency coverage uses original synthetic IRX files to check available
providers, missing library names and their consumer/version/ordinal, unchanged
input bytes, and non-executable archive metadata. Export validation is shared
with the registry and remains covered by the format suite. Static presence is
not a successful library initialization or an ABI-compatibility test.
Startup planning has separate synthetic graph coverage for consumers appearing
before providers, a basic provider followed by a replacement, stable ordering,
compatible minor versions, genuine cycles, wrong-major/insufficient-minor
providers and missing ordinals. It rejects reliance on a shadowed older export.
The IOP interrupt milestone drives original guest exception-handler instructions
through the controller and CPU: masked pending causes, selective acknowledgement,
read-to-disable control, rejected accesses, hardware aliases, Cause/EPC reads,
status restoration with RFE, interruption before a branch delay slot, and exact
branch resumption. It also checks delayed-load retirement, CPU gating and an
explicit stop for the unavailable firmware vector. These synthetic events do not
establish working intrman exports or real device-event routing.
The linked intrman service milestone installs an original provider into an
explicit RAM allocation and executes an original import consumer through the
normal startup and branch-delay path. It covers handler registration/release and
duplicate errors, primary masks, nested suspend/resume, RAM aliases and invalid
output pointers, interrupt-context rejection, callback registration/reset and
unsupported DMA/kernel-call ordinals. Invalid callback pointers preserve the
previous registration. A syscall outside the installed trampoline addresses remains unsupported.
Every call must reach its expected return or fault boundary; a zero result alone
does not pass. Registration-module coverage loads the original generated IRX at
two addresses, links its loadcore import, executes the guest call and checks that
the completion marker remains clear before success and after a rejected call.
The optional local startup test also checks native intrman registration by the
disc-supplied module manager before the next diagnostic stop. Registration does
not establish complete interrupt services or a successful game boot.
The DMA service milestone links an original consumer to dmacman alongside
intrman through the shared provider router. It checks both register banks,
MMIO/service coherence and address aliases, tag registers, per-channel priority
and enable preservation, and shared global-enable state. Unsupported transfer
starts, interrupt-control operations, channels and register widths must stop
without changing channel state. Compiled dmacman imports use that same register,
priority and enable behavior when the diagnostic device set is bound. The
production context binds only the two priority words: priority and enable
update those words, slice setup and transfer starts stop without a completed
transfer or a register write, and channel or control ordinals stay unsupported. A constant-address loaded pointer calls
only its proved targets, and a null pointer does not fall through the jump
delay slot. An unbounded store through a fixed slot may call any instruction
compiled into that program, and every other address stops as unmapped code;
proved loaded pointers stay on their smaller set. A load skipped by a branch
stays unresolved, and a known-address store in another bundle module poisons
that slot, opens it, or widens its executable targets. A callee reached only
through that proof is removed once the slot is open. A fill, copy, or move
with one constant destination and byte count poisons only that span. An
unknown-address store leaves every single-slot load in the bundle open unless
a stack-relative address, a known address, or a constant span excludes that
slot. A store reached only through that pointer still leaves the call open. The local startup regression requires both
native registration markers; it does not claim completed transfers or a boot.
The heaplib milestone executes linked guest calls for fixed and expandable
heaps. It checks alignment and non-overlap, free-size accounting, coalescing,
reuse, cross-heap/interior/double-free rejection, RAM aliases, user-data
preservation, backing exhaustion, integer bounds, deletion and failed backing
release. Growth must use the requested allocation policy and release empty
expansion blocks. The optional local regression checks registration by the
disc module manager; it is not evidence that every heap operation has executed
in the commercial startup path.

The content policy checks current source candidates and actual Git index blobs.
Ignored local artifacts are not candidates, but force-added artifacts are checked.
It allows only approved source/documentation paths and rejects recognizable
binary content, generated guest-code headers, oversized files, and output names.
New legitimate source locations require an explicit policy update and review.
This is not a copyright detector: human provenance review remains required.

Run the policy again after staging and review `git diff --cached` before any
commit. Nothing about a private repository permits proprietary game content.

## Opt-in local startup regression

Generate and build a local diagnostic executable as described in README.md.
Configure its paths and an expected diagnostic boundary explicitly:

```powershell
cmake -S . -B build `
  '-DANYPS2_TEST_PROBE=C:/local/output/build/Release/ps2-native.exe' `
  '-DANYPS2_TEST_IMAGE=C:/local/output/boot.elf' `
  '-DANYPS2_EXPECTED_STOP=stop=syscall pc=0xADDRESS'
ctest --test-dir build -C Release -R local-startup --output-on-failure
```

Replace the example paths and address with your local output and verified
expected boundary. The default instruction budget is one million; configure
`ANYPS2_TEST_BUDGET` for longer bounded runs. The test requires
diagnostic exit code 3 by default, checks the supplied regular expression, and stores output
in ignored `build/local-startup.txt`. It fails on setup errors, budget exhaustion,
timeouts, crashes, or a changed boundary. Disable it with
`cmake -S . -B build -DANYPS2_TEST_PROBE=`. Never commit local captures or logs.

Probe exit codes are 1 for setup/runtime errors, 3 for diagnostic stops, and 4
for budget exhaustion. A passing startup regression is not proof of game boot.
Set `ANYPS2_TEST_DISC` to the user's ISO to enable the read-only FILEIO server.
It is passed as a separate `--disc=...` argument, including paths with spaces.
Leave it empty for tests of the unmounted endpoint.
For an intentional bounded-execution regression, explicitly configure
`ANYPS2_EXPECTED_EXIT=4` and an expected expression covering the budget stop and
relevant progress counters. This records a repeatable running/waiting boundary;
it does not treat budget exhaustion as successful boot. Other exit codes are
rejected. Restore `ANYPS2_EXPECTED_EXIT=3` for diagnostic-stop regressions.

## Discovering additional code

The recompiler accepts repeated `--entry=0xADDRESS` arguments for observed code
entries. An optional `--capture=DIRECTORY` supplies a matching local RAM capture
for code copied after initial load. Captures contain `ram.bin`, `image-id.txt`,
and `observed-entries.txt`. They are analysis evidence, not a full saved machine
state. The probe's `--capture=NEW_DIRECTORY` option can collect these artifacts
when explicitly requested by a local test harness. Keep all such directories
under ignored `work/`.

Do not turn observed game instruction words into committed regression fixtures.
Construct a small original synthetic program reproducing the relevant behavior.

## Latest local GT4 boundary

Earlier local runs with the user's ISO mounted passed CSR initialization
and GsPutIMR, completes SetGsCrt for interlaced NTSC output, VIF0/VIF1 reset,
VU control initialization, VIF error masks, GIF reset/mode setup, graphics DMA
configuration, FPR transfers, VADD/VSUB including broadcasts, VMOVE, VMR32,
VU integer-register setup, scalar comparisons and finite scalar multiplication.
They also passed initial VIF0 DMA transfers, SleepThread, interrupt wakeups and CP0
performance-counter setup, GIF configuration and GS image uploads. The current
run instead executes the disc-supplied IOP memory bootstrap and stops at unsupported
module-manager startup after completing CD/DVD initialization and SifStopDma without the
old bind-retry delay. Rendering milestones separately cover
filtered PSMT4 sprite drawing, vertex assembly, scalar add/subtract, both conversion directions,
scalar minimum/maximum, packed interleaving, VIF buffer-address setup, FLUSH and DIRECT.
It retires 7,965,298 instructions and services
211 kernel calls with a thirty-million-step budget. Fifteen interrupt dispatches
completed and eight IOP packets were consumed. Scanout behavior also has dedicated
synthetic coverage; no visible output is established by these counts.

The guest binds to FILEIO on its first attempt and completes its version query.
It then passes additional startup functions, unaligned word operations, packed
string operations, OSD2 byte reads and scratchpad accesses. At the current stop it has
issued one FILEIO request (version), zero successful opens, and zero bytes read.
It has attempted two binds, most recently to the CD/DVD initialization RPC service.
The supported service removes the earlier retry loop; a lower retired count here
reflects a different startup path, not a loss of tested rendering support.
The local diagnostic reports the CD/DVD initialized state explicitly.
Actual file reads are verified by the synthetic disc/RPC integration test;
the game has not yet exercised them. Fewer instructions than the previous
300-million-instruction wait reflect removal of the FILEIO bind retry loop.
The local regression again expects diagnostic exit 3, with a thirty-million-step
budget and checks for the unsupported instruction stop, instruction count and FILEIO counters.

The existing hardware-register integration suite covers GS reset and identity,
display register lane writes and aliases, IMR access through MMIO and HLE,
empty-FIFO flush/acknowledgement, and rejection of unsupported transfers and
accesses without changing device state. Scanout coverage extends this milestone
with NTSC blanking edges, delayed GS field events, masking/unmasking, acknowledgement,
alternating fields, PAL phase preservation across fragmented advancement, invalid
mode rejection and reset cancellation. Before configuration no video events occur.
The generated-code suite delivers GS, VBlank-start and VBlank-end interrupts to
registered synthetic guest handlers and checks return/context restoration.

VIF initialization extends the same hardware suite: both units, idle stop/break/
cancel, reset configuration preservation, row/column data, aliases and isolation,
register holes, unsupported widths, FIFO access and reverse transfers. Generated
C++ tests execute CFC2/CTC2 control roundtrips, per-unit VU resets, preservation
of upper GPR lanes, idle status and rejection of microprogram activation.

The graphics initialization milestone covers GIF reset/pause/mode readback,
preservation of GS and DMA configuration, independent channel setup, QWC width,
priority-control rejection, invalid accesses and absence of fabricated completion.
Generated LWC1/SWC1 tests preserve raw bit patterns, adjacent memory and GPRs,
and check misalignment faults including a branch delay slot.

The hardware milestone also exercises VIF DMA enable/suspend gates, completion
interrupts and acknowledgement, normal transfers, NEXT/REF/CNT/REFE/END chains,
tag IRQ termination, TTE, scratchpad data, END tag-address preservation and
configuration commands split across tag/data beats. Unsupported commands preserve
the entire current beat and cannot fabricate completion. Split MPG uploads check
micro-memory wrapping and remaining-count readback. Uploaded code is not executed.

GIF packet integration covers PACKED A_D setup, PRE/PRIM, odd REGLIST padding,
sixteen-register tags, reset isolation, pause/resume, masked SIGNAL/LABEL updates,
FINISH and interrupt acknowledgement. Failed REGLIST beats preserve GS state;
unsupported transfer setup preserves DMA address/count and cannot fabricate
completion. IMAGE payloads without active transfer setup are rejected. These checks verify packet transport
and configuration, not pixels or rendering.

The image-upload milestone verifies actual local-memory words after PSMCT32 and
PSMCT24 uploads: column layout, page/row crossings, pitch, partial pixels across
IMAGE/IMAGE2 packets, preserved high bytes, four-MiB address wrapping, reset
preservation and DMA completion after pixel delivery. These are original synthetic
pixels. They verify local-memory uploads, not texture sampling or rasterization.
VIF address-setup coverage checks ten-bit immediate masking, OFFSET latching BASE
into TOPS, separate staged ITOPS and launch registers, VIF0 rejection of VIF1-only
commands, failed-beat atomicity and command continuation after idle FLUSH.
DIRECT transport extends the hardware milestone with word-offset packet assembly
across DMA beats, pause/retry preservation, DMA completion, contention between
paths 2/3 and EOP ownership release. Synthetic A_D packets verify GS delivery.
Vertex assembly coverage exercises all seven defined primitive types, attribute
capture, XYZF depth/fog handling, draw-suppressed list/strip/fan progression,
fan-anchor retention, PRIM restart and atomic rejection of unsupported draws.
The REGLIST rollback case completes a point to verify that a failed draw does
not commit preceding attribute changes from the same GIF beat.
Sprite rasterization tests check actual local-memory pixels, exclusive right/bottom
edges, XYOFFSET, inclusive scissor clipping, framebuffer masks, FBA and PSMCT24
alpha preservation. A REGLIST beat draws before an invalid second command;
the test verifies that neither pixels nor vertex state escape the failed transaction.
These draws now run through Vulkan. The `vulkan-rendering` milestone enables the
texture path with original 2x2 data: repeat/clamp/region modes, MODULATE/DECAL,
TCC, TEXA/AEM, reversed U direction and overlapping texture/framebuffer memory.
A GS-level test connects host upload, register decoding and captured UVs to GPU
pixel output. Bilinear coverage checks all 256 pairs of four-bit interpolation
weights for direct and indexed textures, half-texel positioning and palette
expansion before filtering. Perspective and mipmapped textures remain unsupported.
PSMT4 GPU tests cover nibble, column, block and
page address boundaries with synthetic indices and palette colours. GS tests
cover CSM1/CSM2 palette loads, CSA banks, both remembered bases, conditional
reloads and CLD=0 preservation. The same `vulkan-rendering` milestone enables the
Khronos validation layer, dispatches the compute shader and compares all four MiB
against an independent table-based CPU oracle, covering masked writes, page
crossings, VRAM wrap and aliased framebuffer addresses. It requires a GPU/driver
and installed validation layers. Configure `ANYPS2_TEST_PRESENTATION=ON` to add
the visible three-frame swapchain test; it is disabled by default for unattended
environments. Both tests fail on Vulkan validation errors.

The vector arithmetic milestone executes generated QMTC2/VSUB/QMFC2 sequences
for every lane mask and an aliased destination. It checks finite positive and
negative results, cancellation, signed zero, underflow, denormal inputs, tiny
subtrahends, rounding toward zero, MAC lane bits and sticky STATUS flags. Disabled
lanes may hold unsupported exponents without faulting. Exponent-255/overflow stops
must preserve all destination lanes and flags. VF0 writes, masked VMOVE, aliased
VMR32 and raw LQC2/SQC2 roundtrips are included in the same compiled integration
suite. A deterministic 512-case finite corpus also compares against independent
exact-double subtraction followed by rounding toward zero; its bounded exponent
separation keeps the reference operation exact. This does not establish complete
hardware numeric behavior or pipeline timing.

The same milestone covers all lane masks and all four broadcast selections for
addition and subtraction with an aliased destination. Generated CTC2/CFC2
roundtrips exercise every integer register, truncation and VI0 invariance.
Scalar move/sign operations cover raw payloads and flag preservation. Comparisons
and all four BC1 branch forms cover ordering, denormal zero, exponent-255 clamping,
taken and untaken paths, delay-slot execution and likely-branch annulment.
Scalar multiplication extends this generated-execution milestone with aliased
destinations in a branch delay slot, sign/denormal/underflow behavior, sticky and
current flags, and failure atomicity for unsupported extended ranges. A 512-case
finite corpus compares against exact double products rounded toward zero. This
validates the implemented numeric model, not PS2 multiplier precision quirks.

Scalar conversion/division extend the same compiled milestone: signed integer
limits, chop rounding, destination aliases, conversion in a branch delay slot,
nearest division, zero/denormal operands, sticky/current flags and atomic rejection
of extended numeric ranges. A bounded 512-pair finite division corpus compares
with independent double-precision quotients rounded to float. Conversion leaves
FCR31 unchanged; division preserves flags outside its I/D updates.
Scalar ADD.S/SUB.S reuse the VU numeric core and are exercised with aliases,
signed zero, cancellation, denormal inputs, underflow flags and atomic overflow
rejection. Existing VU corpus coverage continues to verify the shared core.
CVT.W.S tests positive/negative fractions, denormals, both signed integer limits,
exponent-255 saturation and flag preservation through generated code.
MIN.S/MAX.S coverage compares both operand orders for positive/negative values,
signed zero, denormals and exponent-255 patterns, with aliased destinations and
preserved sticky/unrelated flags.
Packed interleaving extends the same generated-code milestone with all six
lower/upper byte, halfword and word forms, checking every output byte with an
aliased destination. Fixtures use original synthetic lane patterns.

Synthetic generated-code tests separately verify thread switching and restarting,
full register preservation, priority changes, equal-priority ordering, and
syscalls in delay slots. Sleep/wakeup coverage extends this milestone with queued
wakeups, cancellation, ready/sleeping status and priority selection. A generated
SleepThread in a branch delay slot exhausts a bounded idle budget without retiring
mainline instructions, then resumes through a timer interrupt and generated
iWakeupThread callback with its register state intact. Idle device steps consume
the run budget but do not increment the diagnostic retired-instruction count.
Arithmetic tests cover signed division overflow and
zero divisors. Performance-counter coverage extends generated execution with all
selector encodings, ignored control-write aliases, sign extension, preserved upper
GPR halves, read-before-increment behavior, enable/privilege/ERL gating and atomic
rejection of unsupported events and overflow. Coherent-memory miss counts are an
explicit approximation, not validated PS2 cache behavior. Other arithmetic covers
unsigned result sign extension, PLZCW lanes, and every byte offset
for word and doubleword partial loads/stores. SIF tests cover payload movement, tag chains, buffering,
handshakes, and malformed inputs. There is no verified visible game boot.
Accurate scanout timing, VIF/GIF rendering, remaining RPC services, and full IOP execution remain
unfinished; no game-specific patch is used to skip them.

The IOP execution milestone covers timer initialization through a synthetic
guest halfword store, prescaler continuity across execution budgets, both counter
widths, overflow/target polling, read-clear flags, stopped counters and rejection
without mutation. Faulting instructions must not advance the diagnostic clock.

The timer interrupt milestone covers all six controller cause bits, masked
latching, target reset, pulse rearm, repeated toggle, one-shot overflow and a
synthetic guest exception handler acknowledging a timer IRQ and returning by RFE.

The IOP service milestone covers DMA interrupt enables/disables for all thirteen
channels, prior-mode results, completion routing, flag preservation, selective
acknowledgement, force IRQ and invalid output pointers. Completion is injected
through the device interface; this does not verify DMA payload movement.

The disc reboot integration fixture includes an original module manager that
polls an EE-owned SBUS flag. The reset packet completes while the manager waits;
a subsequent EE write releases it. Tests verify preserved EE state, suppression
of HLE readiness during reboot, absence of BOOTEND and stable stopped execution
after a fault. DMA register tests cover halfword merging and START rejection.

Guest SIF1 milestone coverage includes FIFO starvation, priority gating, partial
payloads, pause/resume, nonterminal and terminal tags, completion acknowledgement,
empty packets and invalid extents with no partial mutation. The synthetic disc
manager now enables SIF1 after its handshake and waits for an actual EE payload;
the integration test verifies all payload words reach reboot RAM, not HLE RAM,
and the submitted job completes before the manager returns.

The IOP CD/DVD initialization milestone covers guest byte reads through KSEG1,
NOP completion routing and acknowledgement, parameter errors, read-clear error
state, subcommand result draining, bounded parameter capacity and rejection of
unsupported commands without consuming pending parameters.

RTC coverage uses an injected time source to verify BCD encoding, UTC+9 date
rollover, leap-day progression, response latching and FIFO exhaustion.

The IOP partial-word milestone checks every byte offset, RAM aliases, both pair
orders across word boundaries, delayed consumers, budget splitting, MMIO
rejection without side effects and isolated stores. The native-execution test
also covers the EE callers when the shared merge helper changes.

CD/DVD command coverage also verifies the public mechanical-profile byte order,
response exhaustion, required parameter count and power-off-ready cancellation.
These queries do not establish media or firmware-command compatibility.

The IOP resource-directory milestone covers metadata-only defaults, original
payload layout, invalid and duplicate names, aligned bounds, guest address aliases,
read-only access and rejection of instruction fetching from the resource window.
Run iop-execution and disc-fileio-rpc for changes to this bootstrap device path.

The IOP context-switch milestone executes an original guest scheduler callback,
checks same-thread resumption and selection of a full new context, delayed-load
retirement, saved registers, interrupt control, split budgets, invalid frame
targets and stacks, and isolation from unimplemented native service entries.

Native IOP primary-interrupt coverage exercises handler userdata, acknowledge and
mask behavior, both handler return outcomes, split budgets, pending loads, exact
branch-delay resumption, unregistered-source stops and guest-requested preemption
to a different full thread context. Run iop-execution and disc-fileio-rpc for this
CPU/provider integration; commercial startup probes remain separate local checks.

The kernel-callback milestone checks both the native export and syscall entry,
three-argument forwarding, pending-load retirement, callback results and memory
effects, nested calls across single-instruction budgets, restored status and
continuations, invalid stacks/targets and inactive return-gate rejection.

The DMA cascade regression covers simultaneous completions in both banks,
per-channel acknowledgement, chained guest execution across budgets, zero-return
mask changes, final aggregate-latch clearing, context restoration and preservation
of unregistered pending completions.

The SIF0 sender milestone checks priority and FIFO stalls, pause/resume, payload
order, trailing-lane padding, completion flags and atomic rejection of bad extents.
The synthetic disc-reset manager now echoes an EE SIF1 payload back through guest
SIF0 source-chain registers into EE RAM, exercising the shared transport end to end.

## LLVM ahead-of-time milestone

With Clang available on Windows x86-64/MSVC, `iop-llvm-execution` compiles emitted
LLVM IR at `-O2` and links it into the same data-only RAM execution regression used
by the C++ backend. Coverage includes direct calls/returns, aliases, arithmetic,
branch/load delays, byte/halfword/word memory and fault stops. It links no interpreter
or LLVM runtime. Signed comparison and branch coverage includes SLT/SLTI and
BLTZ/BGEZ/BLEZ/BGTZ across zero, positive, negative and signed-limit inputs, with
taken/untaken paths and budget suspension. Branch-and-link REGIMM forms remain
unsupported. The test uses generated synthetic routines in both backends.
Shift coverage includes arithmetic-right immediate shifts and all three variable
shift forms. Zero, sign-bit, signed-limit and negative inputs are combined with
counts at and beyond the word width, checking five-bit count masking and sign
extension while suspending between instructions. All fixtures are original.
The same process invokes the native modload boot-device query on a private RAM
image: `cdrom:`, `cdrom0:`, and `rom:` through `rom3:` are legal; other
well-formed paths are illegal; a null or unterminated pointer does not return
success. The query does not create loader callback state or start a module.
LoadStartModule on a synthetic disc returns -201 for a null or non-disc path
and -203 when the image is absent. It stops without a module id or result write
when the opened bytes are absent from or present in the AOT source catalog
without a sealed entry. A sealed entry whose installed image matches calls that
compiled code: the caller receives the module id and the guest result word
receives the entry value. Entry value 1 releases the image. Any other value
stays resident, and a second start returns -206. StartModule returns -206 for
that resident id and -202 for an unknown or released id, without reading the
name or the result pointer and without calling an entry. A wrong installed image and an
overlapping reservation do not start the entry. A held heap span is skipped by
ordinary allocation. Mode 2 at that address and rounded size claims it and
returns the base. A catalogued file with no sealed record stays unavailable.
After the sealed record is attached, the same call reaches the compiled entry
and finishes with the module id.
HI/LO coverage verifies initial state, signed/unsigned 64-bit products, explicit
HI/LO writes and readback across one-instruction and larger slices. The LLVM
adapter uses ABI entry version 2 for the enlarged state buffer; rebuild generated
objects and their hosts together when changing the ABI.
Division coverage uses explicit quotient/remainder expectations for zero divisors,
signed overflow, all sign combinations and high unsigned operands. Both backends
run the same fixture with several slice sizes. Signed division widens to 64 bits
and the LLVM backend substitutes a nonzero divisor before evaluating division,
so exceptional guest inputs cannot introduce host undefined behaviour or poison.
BREAK coverage checks guarded normal paths, explicit trap codes, delay-slot traps,
pending-load preservation and suspension immediately before a trap. Discovery
does not decode unreachable bytes beyond a terminal trap. The native backend
returns a diagnostic stop without retiring the BREAK; exception-handler delivery
is not implemented, and this is not a successful execution result.
Native-service integration uses an explicitly synthetic provider to verify
argument/result routing, resuming compiled continuations, pending-load retirement,
slice accounting and exactly-once invocation. It also checks missing providers,
unsupported requests, provider faults and rejection of delay-slot services.
It does not claim implementation of PS2 context switching or syscall services.
The native fixture's shift, multiplication and division routines are discovered
through an original export table rather than explicit entry roots. Both compiled
backends execute them; malformed export targets must fail before output. The
format suite separately checks the shared export parser and library registry.
The LLVM execution fixture additionally compiles and runs a 1,102-instruction
routine across five bounded execution functions. Branch/delay-slot and pending
load pairs straddle function boundaries. Slice sizes of 1, 255, 256, 257 and 2,048
verify retirement counts, state, data and return values across all RAM aliases.
The C++ fixture remains within its explicit backend size limit.
Indirect-transfer fixtures materialize original routine addresses and execute
register calls/jumps through both backends. They check aliases, return links,
delay slots and suspended execution. Unknown entry-state and memory-loaded target
variants must reject conversion without partial output, while an unexpected
runtime target must stop before changing the return link or retiring the call.
Checked ADD/ADDI/SUB coverage compares compiled results against wider arithmetic
over zero, sign and signed-limit operands. Overflow must preserve PC, destination,
pending loads and delay-slot state, even when the destination is register zero.
LLVM uses signed-overflow intrinsics; the C++ backend uses unsigned arithmetic
and sign-bit checks. Neither relies on undefined signed host overflow. Overflow
currently reports a diagnostic stop rather than dispatching a PS2 exception handler.
An original sysclib import table and caller verify native `memset` lowering through
both backends. Checks cover alias pointers, truncation to the fill byte, untouched
neighbours, returned pointers, zero lengths, atomic bounds failure and code-write
protection. An unsupported import ordinal must fail before emitting LLVM output.
`iop-llvm-project` exercises the default CLI backend through
IRX conversion, native object compilation, executable linking and execution after
the original IRX is removed. It also checks a `sealed@` module: the record stays
out of the startup image, entries, and source catalog, and `sealed.data` holds
the relocated initialized bytes. An unaligned base, a raw overlap, a gap inside
the startup span, and `sealed@` combined with `romdrv@` fail before output is
created. The C++ pipeline remains covered with `--cpp`.
Run `ctest --test-dir build -C Release -R '^iop-(llvm|native)-' --output-on-failure`
after building `iop-llvm-tests`, `iop-native-tests` and `ps2-iop-recompile`.
These tests do not establish game booting, complete module support or performance.

Native memory-copy coverage uses original import callers for memcpy and memmove.
It verifies copied data, caller return, alias pointers, pending source loads,
forward/backward/identical overlapping moves and empty operations. Invalid source
ranges, protected destinations and overlapping memcpy fail before any destination
mutation. Both compiled backends run the same regression.
The standalone IRX pipeline contains original sysmem allocate/free and sysclib memset imports and a
caller that fills a byte, reads it back and returns 43. The test configures and
links the emitted project, removes the input IRX, and checks the executable's
result and 12-operation count for both backends. Stub protection also checks writes
to the replaced ordinal/delay word and rejects execution starting at that word.

Native allocator integration uses original sysmem import stubs and a compiled
caller on both backends. It covers sliced call/return, pending loads, native fill
of allocated memory, rounded first/last/fixed allocations, exhaustion, free-range
coalescing, reserved regions, invalid frees and queries, and absent providers.
No console allocator instructions or guest allocator metadata are used.

Finite indirect-call analysis is covered by a synthetic branch-selected pair of
callees compiled by both backends. Execution checks both paths, RAM aliases,
single-operation slices, rejection of unproven destinations and trapping delay
slots. An unbounded pointer-producing loop must fail conversion without partial
output rather than truncate its target set. Unknown incoming paths remain errors.

Native library registry integration executes compiled register/release imports,
checks lookup visibility and executes a resolved compiled export. It covers ABI
major/ordinal rejection, duplicate/older versions, newer-version precedence and
release restoration, unknown/mutated declarations and invalid pointers. The
standalone IRX test now registers a converter-emitted export descriptor before
allocating, filling, reading and freeing memory; registration failure traps. Both
LLVM and C++ standalone builds must return 43 after 31 native operations.

Cross-module compilation uses two original, disjoint synthetic modules. The client
stops before provider registration, then resumes through an imported native call
and returns the expected value. Both backends verify pending arguments and sliced
execution, release-in-use rejection, unlink/release, failed relink and successful
explicit relink. Bundle validation rejects overlapping modules, incompatible
provider ABIs and unsupported provider instructions before emitting output.

Both standalone project tests also convert two original IRX files through the
bundle CLI, build a single executable, delete the inputs, and verify ordered
provider/client startup with shared native registration. The provider returns 43
after 31 native operations; the client calls its compiled export and returns 43
after 11 operations. Overlapping layouts and missing providers must fail before
creating output directories. Single-module packaging remains covered in each run.

Loader-query integration executes native imports through both generated backends.
Boot coverage includes empty context, first duplicate selection, full-width mode
IDs, public record layout, a compiled load through the returned pointer, atomic
configuration failure and rejection of mutated lengths. Library-table query
coverage includes registration/release, version precedence, ABI-major rejection
and truncated identity pointers without instruction retirement.

Native disc integration runs compiled ioman imports over an original synthetic
ISO and checks cross-sector reads, RAM aliases, seeking, short reads/EOF, close,
read-only errors and disc-path isolation. Invalid RAM/code destinations must leave
both data and cursor unchanged. The standalone bundle adds a third module that
opens SYSTEM.CNF, reads one byte and closes it. It stops without `--disc` and returns
the fixture byte with an image mounted after the source IRX files are deleted.
The existing disc-fileio-rpc regression covers reuse of the shared handle backend.
A local opt-in LLVM run also read the first SYSTEM.CNF byte from the supplied GT4
image and matched the independent parser read; it executed synthetic modules and
is evidence of native disc access only, not converted GT4 execution or boot.

Native interrupt integration discovers a callback from a constant registration
argument and executes it through both backends. Coverage includes duplicate
handlers, global/per-cause masks, nested suspend tokens, pending-event retention,
context queries, zero-budget and sliced callbacks, foreground HI/LO/pending-load
restoration, delay-slot deferral, zero-return disabling and visible callback traps.
Unknown handler addresses and code-overwriting state outputs are rejected without
retirement. Callback data is placed outside the large LLVM fixture's code ranges.

Bundle callback coverage registers and executes a non-exported handler from another
module through both native backends. Reversed module ordering exercises revisiting
an already analyzed owner. Unaligned, unowned and BSS callback roots reject
conversion without partial generated output.

Native event integration runs compiled imports through creation, AND/OR polls,
keep-mask clearing, clear-on-success, status and deletion. A compiled interrupt
handler signals the same event through iSetEventFlag, then foreground code
consumes it. Coverage includes full result patterns, initial-state retention,
reserved status words, invalid modes/patterns, context rejection, stale IDs and
code-write rejection without consuming bits. Without a connected scheduler, an unsatisfied wait must stop at its
import without retirement.

The native sleep/wakeup milestone runs two compiled continuations through sleep,
foreground wakeup and completion, and separately wakes an idle scheduler through
a compiled interrupt handler. Single-operation slices verify callback retention,
thread HI/LO/register preservation, exact retirement and aggregate execution
accounting. An idle wait must consume no operations, and finished threads cannot
be revived. Early wakeup counts, cancellation and compiled thread identity are
also checked. Standalone LLVM/C++ project tests exercise host scheduler admission.

Scheduler-backed event tests execute compiled WaitEventFlag calls and interrupt
signalling across single-operation slices. Multiple waiting continuations verify
FIFO clear-on-success selection, deletion results and single-waiter exclusion.
Status must count suspended continuations. Standalone project execution verifies
that the host keeps its scheduler and event-service lifetimes shared across entries.

Native thread lifecycle coverage executes compiled CreateThread, StartThread and
DeleteThread imports against the shared native allocator. It checks aligned stack
reservation and default fill, worker argument/GP/stack setup, cooperative priority
selection, restart after return, clear-on-delete, released capacity, stale handles
and rejection of uncompiled entries before allocation or retirement. Both emitted
backends execute the lifecycle; standalone project tests cover the shared host.

Explicit native thread-exit coverage runs compiled ExitThread/ExitDeleteThread
calls with a BREAK immediately after each call. Both must retire once and stop
without reaching it. Budget slicing immediately before exit checks zero-budget
immutability and deferred cleanup. Tests also cover dormant restart/deletion,
clear-on-delete, stack capacity recovery, caller resumption and interrupt-context
rejection on both native backends.

Native argument-block startup executes compiled StartThreadArgs and a compiled
worker that reads the copied block. It verifies byte count, destination alignment,
SP/FP setup, aliased source addresses, independence from later caller writes and
overlapping copies. Invalid source ranges and oversized counts must preserve the
dormant worker and leave the import unretired. Both native backends run this path.

Thread-entry discovery coverage removes the argument worker from explicit roots.
A compiled CreateThread call points through an aliased address to a descriptor
in another module, whose aliased entry points back to that worker. Both generated
backends create/start it successfully. Reversed bundle ordering is covered, and
an invalid descriptor entry must reject conversion before emitting partial code.

The native system-status milestone queries GetSystemStatusFlag through compiled
imports in foreground and interrupt context. Status checks preserve zero initial
bits while supplied boot-mode data selects current bits. A compiled waiter blocks
on an unset bit and a compiled interrupt handler signals that same shared event.
An empty boot context must leave status bits unset; unrelated event allocation
must not change the returned handle.

Native alarm integration uses an injected monotonic tick source and compiled
registration/callback code. It checks minimum-interval writeback, duplicate and
context errors, masked/early/zero-budget delivery, single-operation callback
suspension, one-shot removal, periodic catch-up relative to the previous deadline,
cancellation, and waking an idle sleeping thread. GetSystemTime retains both words
of one clock sample; unknown handlers, protected interval output and deadline
overflow must not register an alarm. USec2SysClock and SysClock2USec use the
same frequency in either context. One million microseconds is one second at
the default frequency, and 36864 ticks is 1000 microseconds. An overflowing
second count and a bad or code pointer leave the outputs unchanged.
Standalone project builds include the host clock and idle-wait integration;
deterministic alarm semantics do not claim exact wall-clock delivery latency.
The production context also serves cdvdman ReadClock and the secrman handler
registrations. ReadClock returns one and an eight-byte BCD clock with clear
status and pad. A year outside 2000–2099 leaves the buffer unchanged. AuthCard
returns 0, and registering or clearing a handler does not call it.

Native SIF integration executes compiled transfer/status/handler imports against
an explicit synthetic EE data arena. It checks multi-descriptor copies, aliases,
completed tokens, masked and sliced callbacks, void completion results, handler
reset, and callback-source ownership when alarms and transfers are both ready.
Invalid later descriptors, protected EE destinations, unsupported attributes and
unknown completion handlers must leave the entire batch unchanged. An absent EE
endpoint must stop without retirement or fabricated success. This is a native
transport test, not execution of converted EE game code.

Native SIF control integration executes compiled initialization, status and address
imports. It checks disconnected/not-ready peers, actual readiness publication,
acknowledgement directions, repeated initialization and command-buffer aliases.
An explicit incoming payload must be visible before a compiled SIF1 handler runs;
masking retains the notification, and SetDChain must rearm before a second receive.
Invalid/protected destinations preserve reception readiness and data. This fixture
publishes a synthetic peer explicitly and is not evidence of converted EE startup.

Compiled native printf coverage crosses register and stack variadic arguments,
including signed minimum, unsigned maximum, hex prefixes/zero padding, string
precision, characters, 32-bit long, dynamic width/precision, alternate octal zero
and literal percent. Unsupported formats, unterminated out-of-range strings and
wrapped stack addresses must not retire or emit partial text. Host sink failure
returns an error through compiled execution on both native backends.

Native SIF teardown coverage invokes the compiled deinitialization import with
pending completion work, verifies cancellation without undoing committed bytes,
and checks disabled/released SIF0 handlers, preserved CheckInit, repeated cleanup,
disconnected cleanup and refusal of new transfers/rearming after shutdown.
An active completion or interrupt context must reject cleanup without partial
state changes. Both LLVM and optional C++ generated execution share this fixture.

The conversion fixture checks multiple unresolved calls in one control-flow path
on both backends. Unknown calls must not hide a malformed delay slot or an
unsupported instruction in the return continuation. Every failed conversion must
leave its output empty; existing bounded-target, widening and compiled-execution
fixtures remain part of this compiler milestone.

Native SIF command integration compiles registration and a private two-argument
handler on both backends. It verifies discovery from AddCmdHandler, user/system
table strides, reserved-word preservation, aliases, packet visibility, masked
and sliced callback delivery, foreground restoration, removal and mutable-target
revalidation. Busy reception, malformed headers, extra-data packets, invalid
indices/extents, protected code and packet/table overlap must not partially
modify state. This is synthetic native command delivery, not an EE handshake or
commercial-game boot test.

Outbound command integration executes compiled SendCmd/iSendCmd imports with a
synthetic peer arena. It covers stack-passed extra-data arguments, header fields
and preserved options, exact byte lengths, aliases, interrupt context, peer
backpressure, owned packet notifications and increasing success tokens. It also
checks overlapping caller buffers, nonpositive extra-data sizes, disconnected
peers, invalid packet lengths, stack overflow and protected/out-of-range peer
destinations. Rejected sends cannot partially write extra data or command bytes.

The shared SIF completion milestone mixes compiled raw and command transfers and
checks consecutive tokens and completed status through the same service. It
executes general and per-command callbacks under masking and budget slicing,
rejects unknown callback addresses before writes, and verifies that a busy peer
does not enqueue a duplicate completion. Teardown must reject active callbacks,
cancel pending command callbacks, stop new sends and retain committed peer
notifications. Both callback-send variants are exercised with stack arguments.

The command-session milestone runs compiled InitCmd through the native scheduler
and proves it remains suspended until an explicit peer command acknowledgement.
It checks separate command/RPC phase bits, peer flag acknowledgement, subsequent
outbound buffer selection, repeated initialization, compiled Get/SetSreg and peer
register writes. Disconnected/unready/stopped links, absent wait continuations,
protected peer addresses, invalid register indices and a deleted shared status
event must not fabricate initialization or partially publish peer state.

Unified inbound integration queues the actual command-init acknowledgement through
the native callback multiplexer and scheduler. It must remain pending under an
interrupt mask or zero budget, then wake the suspended compiled InitCmd caller.
User-command coverage checks owned packet storage, backpressure, branch-delay
boundaries, active-callback lifetime, sliced execution and explicit system-table
overrides. Mutating a handler after packet acceptance must produce a deferred
execution fault without changing packet RAM; the fault remains visible until an
explicit discard, after which later valid packets can execute normally.

RPC registration integration executes compiled queue/server imports and discovers
and runs a private RPC function through the converter. It checks O32 stack
arguments, alias-preserving links, untouched request fields, insertion/removal
order, wrong-queue removal, detached queue cleanup/republication and descriptor
reuse. Duplicate/overlapping records, uncompiled primary/secondary functions,
protected/out-of-range storage and interrupt-context calls must not partially
modify registration state. This milestone does not enqueue or execute RPC calls.

Native RPC bind integration registers servers through compiled imports, queues a
synthetic peer request through the interrupt-gated inbound router, and inspects
the actual native peer reply. It checks packet/client identities, server/buffer
fields, unknown services, insertion order and exclusion of detached queues.
Backpressure retains an unchanged reply snapshot across descriptor mutation;
protected reply destinations and mutated uncompiled handlers must produce
explicit faults without a partial peer response.

RPC admission coverage queues owned payloads through the inbound scheduler, wakes
a sleeping compiled worker and executes compiled GetNextRequest. It checks exact
request fields, original pointer aliases, FIFO links/order, active/empty queue
state and absence of fabricated replies. A mismatched payload destination and a
second request for a busy server must leave prior data and queue state intact;
removal cannot discard requests awaiting execution.

ExecRequest integration enters a compiled three-argument RPC handler, slices its
execution, validates its result and inspects the peer's actual reply/payload.
A full peer mailbox must stop at the native return boundary without rerunning
the handler; changing input RAM during that wait cannot change the saved reply.
The fixture checks caller PC/register/GP restoration, unchanged execution counts
on retry and server release after successful delivery. Invalid reply modes and
uncompiled replacement functions cannot enter the handler. Polling-mode delivery
writes result bytes and clears the completion IDs at the original packet address
while preserving an unrelated pending command. A protected reply leaves both
peer regions unchanged, retains the completed continuation and can be retried
without reexecution. Both generated backends run this same integration fixture.

The native command-session integration also enters compiled InitRpc before
command setup is complete. It verifies that the first event wait retains the
import PC and operation count, that an RPC acknowledgement cannot satisfy the
command phase, and that command acknowledgement alone cannot finish RPC startup.
It inspects the outgoing RPCINIT packet, rejects duplicate publication during a
retry, and delivers the final acknowledgement through the interrupt-gated inbound
path before verifying caller resumption and idempotent initialization.

The RPC integration enters compiled RpcLoop on a registered worker, verifies an
empty queue sleeps at the import without retirement, and admits two requests.
An uncompiled replacement handler must leave the FIFO and input data unchanged.
After repair, command-notified and polling replies both complete, the FIFO drains
and the worker sleeps again. Reusing a completed server wakes that same worker
and runs exactly one additional handler without a fabricated command reply.

The shared generated-code scheduler regression calls ChangeThreadPriority to
lower its current continuation and verifies two ready workers execute in FIFO
order before that caller resumes. It then raises another ready worker and checks
that the caller yields after exactly one service operation. Dormant-worker,
invalid-priority and interrupt-context requests return errors. Both LLVM and
optional C++ output execute the same scenario.

Compiled ReferThreadStatus is checked across ready, running, sleeping, event-wait
and dormant transitions. The fixture verifies event identity clears on completion,
creation option/attributes and stack metadata survive, current-thread lookup works,
and reserved/context fields are initialized without host pointers. Invalid output
ranges, generated-code destinations and unknown handles must leave RAM unchanged.

DelayThread integration uses an injected steady clock and compiled import. It
checks public wait metadata, no early completion, preserved wakeup credit,
zero-budget behavior, interrupt-masked completion, exact caller resumption and
the minimum wait for a zero-duration request. Standalone generated-project tests
also compile the host's combined alarm/thread deadline handling.

The semaphore integration runs compiled creation, wait, signal, poll, deletion and
status imports. Two suspended workers verify wait metadata and priority selection
after a priority change; deleting the semaphore releases the remaining worker
with the deleted-object result. A second semaphore verifies FIFO selection despite
a later worker's higher priority, including interrupt signalling. Count saturation,
polling exhaustion, stale handles, reserved status words, protected/out-of-range
outputs and interrupt-context errors are checked in the same subsystem fixture.
The existing event regressions verify the shared native wait-interface refactor.

The mailbox integration executes compiled creation, send, receive, poll, deletion
and status imports. It checks priority message order and mirrored circular links,
FIFO delivery, pointer-alias preservation, duplicate-message rejection, protected
receive destinations and reserved status words. Two native receivers suspend;
a changed priority affects direct interrupt delivery, and deletion resumes the
remaining waiter with its public error. Wrong-context sends and stale handles
must fail without fabricating delivery.

The full native diagnostic-format integration now runs through both compiled
stdio printf and sysmem Kprintf imports. It verifies register/stack arguments,
format flags, malformed formats, invalid strings, stack-address overflow and
sink failure without partial output. Null Kprintf control requests must stop
without retirement or sink output.

Compiled GetOtherData integration verifies blocking and NOWAIT native copies,
byte alignment, pointer aliases, completion metadata, preserved descriptor fields
and coexistence with an occupied command mailbox. Out-of-range peer reads,
protected or truncated outputs, descriptor overlap, missing/stopped peers and
unknown modes must reject without partial writes. Zero-byte reads must ignore
unused data pointers and still complete. Both generated backends run the fixture.

Native getstat integration uses an original synthetic ISO with a known leap-day
recording date. Compiled calls verify file/root type and size, all three timestamp
fields, zero high-size, aliases and unchanged output on missing files or invalid
output storage. The following open still obtains the first descriptor, verifying
that metadata queries did not allocate handles. Format and disc-fileio regressions
also cover the shared ISO Entry metadata extension.

Native directory integration extends the synthetic ISO with an empty file and an
empty subdirectory. Compiled dopen/dread/dclose verify shared descriptor numbering,
independent cursors, regular/empty/directory entries, names, metadata, EOF and
handle reuse. Protected/truncated output records must not consume the first
entry. Wrong handle-type operations and closed directory reads return errors.

Compiled disc-mutation calls verify read-only and foreign-device errors for remove,
mkdir, rmdir, chstat and format. Write rejects read-only descriptors, even with
zero count or an invalid source pointer. The integration compares the original
synthetic image bytes, RAM and current cursor after rejection, then continues
ordinary reads from that same handle.

Compiled lseek64 integration covers positions beyond 4 GiB, high-word return and
pending-load replacement, signed relative/end offsets, INT64_MAX/INT64_MIN,
overflow, invalid whence and handles, and EOF reads from a large cursor. Legacy
seek must reject an unrepresentable current position without changing it. Invalid
stack storage must preserve the cursor and unretired pending register state.

### Native disc control boundary

The compiled disc integration exercises ioctl and ioctl2 through real synthetic
import stubs on both backends. It checks invalid descriptors, unknown commands,
stream controls on ordinary files, unchanged RAM/cursors, and explicit stops
without retirement for known unimplemented hardware ioctl commands. Unused
payload pointers are deliberately invalid to verify rejection precedes access.
Run the iop-native and iop-llvm execution/project tests in Release.

## LLVM driver lifecycle integration

The original synthetic module calls AddDrv with a constant initialized descriptor.
Its private init/deinit functions are discovered through that data, not explicit
callback roots. A second synthetic image keeps that descriptor in a saved register
across a host DelDrv import and passes it to AddDrv from the delay slot. Discovery
still reaches both private lifecycle roots. A pointer held in a caller-saved
register becomes unknown across that import. Compiled execution checks budgeted init entry, delayed publication,
rollback, teardown with a negative return, duplicate names, the 16-slot limit,
missing deletion, invalid targets/descriptors, and retained caller GP/load state.
A foreign continuation return cannot consume the pending registration. The normal
LLVM execution regression also covers existing RPC continuations with owner tags.
Standalone-project testing compiles the host with the registry installed. Only
iop-llvm-execution and iop-llvm-project are needed for this native milestone.

## Compiled driver file integration

The LLVM execution milestone registers an original synthetic driver whose private
open/read/close routines are discovered from initialized operation-table data.
It opens that device while a native ISO file remains open, checks unique handles,
device-unit/path parsing and the public file record, reads using driver private
state, rejects code destinations and forbidden access modes, and prevents driver
removal with an open file. Failed open and negative close results must release
heap storage and descriptors; a native ISO open then reuses the freed handle.
Existing ISO reads, directory cursors and 64-bit seeks run through the shared pool.
Run iop-llvm-execution and iop-llvm-project in Release for this milestone.

The compiled file milestone also covers write permission on read-only handles,
read/write round trips through driver-private cursor state, zero-length writes,
source address aliases, source extent faults and transfer-size rejection. Seek
checks forward/backward signed displacement, unchanged cursor on invalid origin,
and preservation of a negative callback result. Synthetic write/seek functions
are discovered through the registered operation table, not added as explicit
entry roots. Run only the LLVM execution and standalone-project tests.

The same LLVM compiled-driver integration now enumerates two independently opened
legacy directories. It verifies a synthetic name/type/size record, output bounds,
alignment and code guards before cursor advancement, EOF with unchanged output,
file/directory handle separation, failed-open rollback and close cleanup. Extended records use an isolated conversion buffer, covered below.

Wide compiled-driver seek is covered by an extended synthetic descriptor and a
private LLVM callback discovered from operation slot 22. It echoes both offset
words and records the stack origin, checking values above 4 GiB, INT64_MIN, signed
manager errors, source-stack aliases and preserved caller/pending-load state.
Invalid origins, overflowed stack addresses and uncompiled callback targets must
stop without changing driver-private state. Existing RPC tests exercise the
shared return bridge after adding v1 to native return requests.

Extended directory integration checks the open flag, isolated 64-byte-aligned
scratch record, type/permission conversion, size/name fields, preserved caller
privdata and output canaries. A budgeted callback is completed separately, then
its return is retried after a truncated-arena fault to verify no duplicate cursor
advance. Entry/EOF/error completions must release temporary storage; EOF/errors
preserve caller output. Exhausting the heap returns ENOMEM without calling dread.


Compiled control integration covers scalar ioctl parameters and signed results,
extended ioctl2 on a directory, O32 stack arguments, aliased input/output buffers,
zero-length unused pointers, missing handles, legacy rejection, oversized input,
truncated input/output, code destinations and overflowing stack addresses. The
private synthetic callbacks must be discovered from descriptor tables and run as
LLVM-compiled code. Run the related LLVM execution and standalone-project tests.


Compiled path integration invokes synthetic remove/mkdir/rmdir callbacks through
registered operation tables. It checks device-unit and relative-path arguments,
legacy/extended mkdir permissions, negative callback results, released heap and
handle ownership, blocked driver removal during a budgeted callback, and retryable
cleanup after a truncated heap. Existing native ISO mutation rejection remains
covered in the same LLVM integration test. Run only the LLVM execution/project
Release tests for this milestone.


The LLVM device-control milestone discovers a private devctl callback from an
extended operation table, passes all seven driver arguments, and verifies unit,
relative path, command result, payload, restored SP and unchanged caller stack
arguments. It checks legacy rejection, bad input, code output, stack overflow,
frame/payload overlap, size limits and temporary resource cleanup. These are
synthetic commands, not evidence of commercial device-command compatibility.


Extended path tests compile private chdir/sync/umount callbacks discovered from
an extended table. Legacy devices reject all three; extended calls normalize
positive success, preserve negative results and sync flag bits, and release
temporary resources. The existing path-continuation regression covers delayed
completion and cleanup retries shared by these operations. Run the related LLVM
execution and standalone-project Release tests.


Compiled readlink coverage discovers a private synthetic callback through its
extended operation slot, checks aliased pointers, a positive byte count and an
untouched trailing canary, zero length with an invalid unused pointer, negative
callback results and temporary ownership cleanup. Invalid/truncated destinations,
code destinations and oversized output stop before invocation. Legacy drivers
reject the operation. Run the LLVM execution/project Release tests.


The two-path LLVM regression invokes rename and symlink with relative and
qualified destinations through privately discovered callbacks. A synthetic
negative result depends on both path bytes, checking argument routing. Tests
cover legacy rejection, device/unit mismatch, null and unterminated destinations,
invalid memory without state changes, positive-success normalization and released
temporary ownership. Run the LLVM execution/project Release tests.


The device-type regression queries compiled legacy files, extended directories
and a simultaneously open native ISO file through the shared router. It covers
missing/closed descriptors and rejects a changed registered type without retiring
the call. The LLVM execution/project Release tests exercise the newly bound import.


The compiled mount integration checks all six driver arguments with synthetic
payload output, signed flags/errors, success normalization, original caller stack
preservation and temporary resource cleanup. It rejects legacy devices, invalid
names/payloads, overflowing stack addresses, frame overlap and oversized lengths.
A callback ignoring optional arguments verifies null device names and unused
zero-length pointers. Run the LLVM execution/project Release tests.


Native sysclib classification runs the compiled import for every defined ASCII
byte with nonzero upper argument bits, checking public flags, pending-load result
replacement and unchanged memory. An out-of-domain byte must stop without
retirement. The standalone-project test compiles the host with the new provider.
Run the LLVM execution/project Release tests.


The prnt milestone compiles a privately discovered synthetic character callback,
runs one-operation slices, and checks formatted bytes, final 513/512 controls,
byte count, caller state and ignored callback returns. Empty output still flushes
and resets without reading unused va_list memory. Concurrent calls, unsupported
formats, invalid va_list memory and uncompiled callbacks stop before output.
Existing printf/Kprintf regressions exercise the extracted shared formatter.
Run the LLVM execution/project Release tests.


Compiled string-import integration covers empty/ordinary lengths, aliased
addresses, unsigned high-byte ordering, count-limited comparisons, zero counts
with invalid pointers, early mismatches and terminators at the final RAM byte.
Unterminated boundary reads must fault without retiring the call or changing
memory. The existing LLVM execution/project Release tests cover these imports.


The compiled string-search milestone covers all four import aliases, repeated
matches, first/last selection, pointer-alias preservation, high-byte needles,
missing matches, NUL searches and empty strings. Boundary tests prove first-match
early exit and reject an unterminated last-match scan without returning a partial
pointer or retiring the call. Run the LLVM execution/project Release tests.


The compiled copy milestone checks strcpy terminator/canaries, aliased return
pointers, strncpy padding and truncation, unused zero-count pointers, and a source
terminator at the final RAM byte. Invalid source/destination extents, overlapping
aliases, code writes and oversized counts must fail without partial writes or
retiring pending loads. Run the LLVM execution/project Release tests.


The native fill milestone compiles bzero/_wmemset imports and verifies byte
canaries, full 32-bit values, byte-count rounding, unchanged tails and aliased
returns. Zero-write requests accept unused pointers. Misalignment, truncated
ranges, excessive extents and code destinations must stop without partial writes.
Existing copy regressions cover the shared destination validator. Run the LLVM
execution/project Release tests.


The sprintf LLVM milestone checks mixed register/stack arguments, signed decimal,
zero-padded hex, strings/characters, returned length, pointer aliases, trailing NUL
and surrounding canaries. Empty output at the last RAM byte must succeed without
reading unused stack arguments. Bad destinations, code writes, unsupported
formats, overflowed argument stacks and invalid strings leave output and pending
call state untouched. Existing printf/Kprintf/prnt regressions cover formatter
reuse. Run the LLVM execution/project Release tests.


The strcat milestone exercises an ordinary append, empty source/destination,
aliased pointers, retained prefix/terminator and canaries, and an empty append at
the final RAM byte. Overflowing output, unterminated input/destination, overlapping
prefixes and code destinations must stop without partial writes. Run the LLVM
execution/project Release tests.


The strtol LLVM milestone covers whitespace/signs, automatic octal/hex/binary,
base 36, signed limits, positive/negative overflow with complete digit consumption,
missing digits, prefix-only inputs, default-reference base 1 and invalid bases.
It verifies aliased end pointers and pending-load return state. Invalid input,
unterminated boundary digits, non-ASCII classification, misaligned end pointers
and code destinations stop without mutation. Invalid bases with unused input
pointers do not read memory. Run the LLVM execution/project Release tests.


The compiled library milestone checks reboot-mode updates before registration,
input-bit masking, registration with recorded modes, updates on active libraries,
unchanged resolution, clearing/release, null/unknown tables, mutated exports and
truncated descriptors. Failed requests cannot partially update the mode word.
Run the LLVM execution/project Release tests.

The compiled non-auto-link lifecycle verifies query visibility, failed explicit
linking, untouched unresolved imports, duplicate/null/unknown rejection, release
and normal re-registration, and subsequent compiled execution. Mutated exports
and truncated declarations must leave registration and modes unchanged. Run the
related LLVM execution and standalone-project Release tests.

The native invocation integration discovers a private callback from its constant
argument, executes in bounded slices, and checks argument forwarding, memory output,
return value, GP/SP and event-delivery restoration. Concurrent calls and unknown
code targets cannot replace the active continuation or alter delivery state.
Run the LLVM execution and standalone-project Release tests.

The compiled heaplib lifecycle creates fixed and growing heaps, checks low/high
system placement, eight-byte alignment, minimum allocations, accounting, data
preservation, wrong-heap/interior/double frees, coalescing, growth and growth-block
release. Delete with live allocations must restore all backing capacity. Invalid
handles, huge requests and truncated-memory cleanup are covered. Run the LLVM
execution and standalone-project Release tests, which also exercise existing
system-allocation users after allocator reuse.

The compiled stack-check milestone creates real scheduler-owned worker stacks.
A query at worker entry reports remaining capacity; a synthetic worker consuming
its reserve must stop at the import without retirement. Unrelated continuations
are rejected and interrupt-context queries return zero. Run the LLVM execution
and standalone-project Release tests.

The compiled termination lifecycle detaches one of two waiters for each event,
semaphore and mailbox service. Subsequent signalling must complete only the live
waiter and preserve the cancelled output buffer. Repeated termination reports
dormant status; a ready worker retains its owned stack and can restart and delete
through the existing lifecycle test. Run LLVM execution/project Release tests.

Shifted-target discovery covers all six shift forms, zero and masked counts,
negative arithmetic shifts, and rejection of mutable loaded pointers. The compiled
invocation fixture constructs its private callback with shifts, proving discovery
and LLVM execution together. Run the LLVM execution/project Release tests.

The compiled boot-callback lifecycle registers priorities out of order, drains in
one-operation slices, and checks GP, order, next record, delayed flag, unchanged
deferred status, immediate result output and released scratch capacity. Unknown
callback targets and code status destinations must stop before callback execution.
Run the LLVM execution/project Release tests.

The native SIF integration verifies pre-connection local publication, accumulated
flags and persistence after peer connection, while peer reads and initialization
remain blocked. A local opt-in EESYNC experiment generated LLVM, compiled the
Windows x64 executable and executed its module entry and startup callbacks to
completion with an empty boot context. The entry returned zero after 159 compiled
operations. This is isolated-module execution, not an EE handshake or game boot;
commercial input/output artifacts remain ignored under work/.

The compiled thread milestone constructs a descriptor on its runtime stack and
starts a worker that has no export or explicit root. The worker must execute its
LLVM code. Separate discovery checks reject stale candidates after overlapping
byte writes and unknown-address stores. The local FILEIO experiment first compiled
and stopped at an undiscovered worker during CreateThread. Stack discovery now
reaches worker code and rejects two unresolved indirect calls before emitting
output. Earlier FILEIO entry-only generation did not cover those workers. Run
LLVM execution/project Release tests.

Compiled switch-table coverage selects each case and the default (including a
large unsigned index), preserves an aliased valid destination, and rejects a
mutated table targeting compiled code outside that switch. The local FILEIO
worker investigation identified this bounded compiler pattern at both previously
unresolved transfers; no game-specific addresses enter the recognizer. Run the
LLVM execution/project Release tests.

After switch discovery, FILEIO generates and compiles with worker code included.
The native probe now drains ready workers after entries and startup callbacks,
so a returned module entry cannot hide an unexecuted lower-priority worker.
A drained ready queue is not proof that waiting services or a game have booted.
The FILEIO entry returned zero after 51 compiled operations; draining its workers
then stopped at sifman ordinal 5 (SIF initialization) because the standalone host
has no converted EE peer. No readiness acknowledgement was fabricated.


### EE LLVM execution milestone

Build `ee-llvm-tests` in Release and run `ctest --test-dir build -C Release -R
"^ee-llvm-execution$" --output-on-failure`. The fixture generator rejects unsupported
and inconsistent regions without partial output, emits an original synthetic
scalar program, and invokes Clang to produce the object linked into the test.
Execution verifies word sign extension, 64-bit arithmetic, logical/arithmetic
shifts, bitwise results, preserved upper register halves, register-zero semantics,
budget resumption, stable syscall stops, unmapped PCs and malformed delay-slot state.
The same native executable covers a three-iteration loop, J/JAL and JR/JALR,
call/return links, taken and untaken 64-bit comparisons, delay-slot execution across
budget boundaries, target capture before an overlapping link-register write or
delay-slot mutation, and unaligned destinations. Conversion rejects branches and
syscalls in delay slots before emitting output.
The executable links the test source, generated object and shared numeric primitive plus platform
libraries: no decoder, diagnostic runtime, interpreter or JIT. This milestone
proves bounded EE-region execution only; it does not establish GT4 boot compatibility.


The EE LLVM milestone also executes signed/unsigned byte, halfword, word and
64-bit loads, and byte/halfword/word/64-bit stores through physical and both kernel
aliases. Checks cover little-endian output bytes, preserved register halves,
unaligned/unmapped/device addresses, truncated and null backing storage, and
code-write protection through every supported alias. A faulting delay-slot store
must leave the pending branch intact and complete exactly once after correction.
Loads into register zero must still fault on invalid addresses. Run only the
related `ee-llvm-execution` Release test for these compiler changes.


The `ee-llvm-project` Release test generates an original ELF with separate code
and data segments and BSS. It converts the ELF, builds the emitted CMake project,
removes the input ELF and executes the native result. The program loads initialized
and zero-filled data, stores a result, calls and returns through sparse code, and
stops at a syscall with the expected return value and operation count. Unreachable
unsupported bytes after the syscall do not become executable code. Overlapping
segments, unsupported entry code, existing output directories, operation budgets
and mismatched data are checked. The generated build inputs are inspected for
execution-scaffolding dependencies. Run `ee-llvm-execution`, `ee-llvm-project` and
`analysis` after changes to this shared discovery integration.


The compiled EE milestone now checks packed unsigned saturation for exact-limit,
overflow and non-overflow lanes in both register halves, including destination
aliasing either source and zero-register reads/writes. HI/LO transfer coverage
checks full 64-bit values, preservation of destination upper halves, zero-register
behavior and isolation of both accumulator banks. The related LLVM execution and
standalone-project tests are the acceptance gates.


The EE LLVM milestone exercises shift-amount masking across all low-five-bit
source combinations with nonzero upper bits and a negative immediate. It checks
MFSA upper-half preservation and zero-register behavior. Raw MTC1/MFC1 coverage
uses negative zero, NaN-like patterns and ordinary bits, checking integer sign
extension, unchanged FCR31 and zero-register handling. Both related EE LLVM tests
pass.


The compiled EE floating-add milestone covers normal addition/subtraction,
accumulator writes, destination/source aliasing, cancellation, signed zeros,
denormal operands, chopped precision and signed underflow. Expected values are
checked under all four host rounding modes. Extended operands/results in a delay
slot must preserve the pending branch, accumulator and flags; corrected inputs
then resume successfully. SYNC coverage verifies both accepted modes retire without
changing data state, checks the emitted LLVM fence and rejects a reserved mode.
The shared arithmetic extraction also requires the related hardware-registers
regression because VU finite arithmetic uses the same implementation.

FCR31 transfer tests cover writable-bit masking, preservation of other fields,
sign extension, zero-register handling and rejection of unsupported selectors.
LQ/SQ coverage exercises all 16 address low-bit combinations through physical and
both kernel aliases, distinct register halves, zero-register loads/stores and
address/destination aliasing. Truncated 16-byte accesses and code writes must leave
all memory and destination state untouched.

The current local GT4 milestone generated an LLVM project, compiled it through
`ps2-build`, and initially executed 33,238 native EE operations to SetupHeap. After native heap
setup was connected, rebuilding that existing prefix executed 33,239 operations
and stopped at the first uncompiled continuation instruction. Fresh discovery following SetupHeap now passes the former 4,096-instruction bound
and rejects an undecoded MULT1 instruction before output emission. The larger continuation has
not yet compiled.
Build inputs and DLL dependencies were inspected: generated object, probe, binary
reader and numeric helper, with platform/runtime libraries only. No decoder,
interpreter, JIT or hardware model is linked. All commercial input, LLVM, data and
native output remain ignored under `work/`. This proves initial converted EE
execution, not game boot, rendering, or a playable conversion.


SetupThread integration executes the compiled syscall and its continuation with
one-operation and larger slices. It verifies stack/metadata ownership, returned SP,
empty launch arguments, argument aliases, duplicate setup rejection, and unchanged
state/memory on invalid or code-backed outputs. Correcting a rejected request must
allow one successful retirement. The standalone ELF project test additionally
checks automatic discovery and execution beyond SetupThread to an unknown syscall.
Run the two EE LLVM Release milestones and the related bios-free-kernel regression
when changing the shared initial-stack planner.


The compiled startup lifecycle now covers fixed/automatic/empty heap extents,
EndOfHeap, calls before required initialization, stack overlap and address wrap.
Rejected changes preserve previous heap metadata, memory and retirement state.
One-operation and larger slices must produce the same results. The standalone ELF
fixture discovers, compiles and executes SetupThread, SetupHeap and EndOfHeap
continuations before stopping at an unknown syscall. Related Release EE LLVM and
bios-free-kernel tests passed after sharing heap extent planning.


The partitioning milestone extends the synthetic program beyond 4,096 instructions
and inspects every emitted part for the 512-instruction bound. Native execution
crosses multiple parts with budgets of 1, 511, 512, 513 and 10,000 operations, including
a branch positioned at a proposed partition edge, its delay-slot pause/resume,
a backward indirect transfer to another part, holes and unaligned destinations.
A store from the small startup part into a distant compiled part must stop without
memory mutation. Both EE LLVM Release tests passed. The native dispatcher is
iterative and does not decode instructions or invoke an interpreter/JIT.


The EE LLVM execution milestone now covers EI/DI across all privilege-mode
encodings, EDI/EXL/ERL overrides, event masking, repeated unacknowledged stops,
explicit acknowledgement, and delivery after a branch delay slot across budget
slices. Comparison coverage includes signed boundaries, 64-bit values above the
32-bit range, negative immediates, and preserved upper lanes. Both EE LLVM Release
tests pass. Local GT4 conversion passes EI and SLTU validation and next rejects
PCEQB; no larger project or visible game boot is claimed.


The scalar-control-flow milestone additionally covers MOVZ/MOVN source/condition
aliasing, full-width conditions, destination-zero suppression and upper-lane
preservation. Ten newly supported conditional branch forms are exercised with
zero, positive, high-word and negative operands, budget splits, and faulting delay
loads. Untaken likely branches suppress those faults. Twelve added shift forms
cover zero, sign-bit and mixed-bit inputs with counts at and above each width;
all compile and execute through LLVM. Both related Release tests pass. The latest
local GT4 conversion now rejects MUL.S after passing the conditional-move,
branch-likely and shift operations. This remains conversion progress, not boot.


Floating memory/control-flow coverage now exercises raw LWC1/SWC1 transfers,
FPR0 writes, negative offsets, RAM aliases, alignment/extent failures and protected
code stores. Bit patterns include signed zeros, denormals and exponent-255 values;
transfers preserve them without numeric interpretation. The unsupported-instruction
fixture now uses MUL.S because LWC1 is supported.

Finite comparison tests cover signs, equality, denormal-to-zero behavior,
condition-bit preservation, and explicit extended-range rejection. All four BC1
forms execute across budget slices, with taken and untaken likely branches. Both
EE LLVM Release tests pass. Local GT4 conversion now advances through the floating
memory and control-flow operations and rejects MTC0 before project emission.


Native vector-control boundary tests compile CFC2/CTC2 calls and exercise missing
owners, pending/retry, selector and wait propagation, sign extension, unchanged
upper lanes, low-word writes, integer-zero behavior and a read stalled in a branch
delay slot. Test callbacks are original synthetic service owners, not evidence
of a production VU implementation. Both EE LLVM Release tests pass. Local GT4
conversion now passes these control-transfer operations and rejects SQC2; it has
not produced a larger executable, and the production vector service is unconnected.


The vector-transfer milestone exercises missing/active storage, full 128-bit
round trips, VF0's constant and ignored writes, aliases, unaligned/out-of-range
faults and protected-code stores. Floating move/sign tests cover raw bit patterns,
source/destination aliasing, unchanged MOV.S flags and ABS.S/NEG.S current-flag
clearing. Both EE LLVM Release tests pass. GT4 conversion now passes SQC2 and
MOV.S and next rejects SUB; no larger executable or visible boot is established.


The system/arithmetic milestone tests disabled performance setup, both counter
writes/readback, sign extension, zero-source reset, ignored control encodings,
and mutation-free rejection of active measurement. Integer multiply/divide tests
cover signed extremes, divide-by-zero, overflow, low-word operands and accumulator
isolation. BREAK is tested at ordinary and branch-delay boundaries, including
repeated stops. Both related LLVM Release tests pass. Local GT4 conversion now
passes performance setup, DIV and BREAK and rejects SDL; no larger executable
or visible boot is established.


Finite multiplication tests execute generated code under all four host rounding
modes, covering signs, signed zero, denormal inputs, underflow, truncation, aliases,
and mutation-free extended-range rejection. Vector add/subtract tests cover normal
and broadcast operands, MAC/status, masked lanes, aliased destinations, zero lane
masks and atomic rejection when a later lane is unsupported. Both EE LLVM tests
pass; hardware-registers also passed after extracting the shared arithmetic helpers.
Local GT4 conversion now passes MUL.S and VSUB and next rejects SUB.


Vector move tests execute all 16 masks for ordinary copies and in-place rotations,
including raw extended float patterns, VF0, unchanged MAC/status and unavailable
storage. LDL/LDR tests cover every byte offset, preserved destination bytes, RAM
aliases, paired loads across aligned-word boundaries, upper lanes and faulting
zero-register loads. Both related LLVM Release tests pass. Local GT4 conversion
now passes VMOVE and LDL and next rejects an undecoded MULT1 instruction before project emission.


Unaligned-store coverage includes every byte offset, aliases, cross-word pairs,
zero source, preserved bytes, bounds failures and executable protection. System
metadata tests cover visibility masks, sign extension, zero-source writes and
interrupt delivery after enabling status. PLZCW tests cover sign extremes, zero,
aliased destinations and upper-lane preservation. Both EE LLVM Release tests pass.
Local GT4 conversion now passes SDL, system metadata transfers and PLZCW, then
rejects PCEQB before producing the larger project.


Signed arithmetic tests cover positive/negative overflow, immediate forms, both
word widths, integer-zero destinations, preserved output and an overflow stopped
inside a branch delay slot. Packed-copy tests cover both source aliases, lane
ordering, halfword replication and integer-zero semantics; packed bitwise tests
check both halves with an aliased destination. Both EE LLVM Release tests pass.
Local GT4 conversion passes SUB, PCPYLD and POR and next rejects an undecoded MULT1 instruction.


The packed-data milestone covers equality masks at all three lane widths, both
source aliases, integer zero, every input bit in color packing/unpacking, and all
nine lane-pack/interleave variants. Fixtures now include a separate synthetic
extension region so the original bounded-function boundary remains stable. Both
EE LLVM Release tests pass. Local GT4 conversion passes PCEQB, PPAC5 and PPACH,
then rejects an undecoded MULT1 instruction before project emission.


## EE operation-family batch

`ee-llvm-execution` includes the compiled synthetic regions in
`ee_batch_fixture.hpp` and the combined checks in `ee_batch.cpp`. The batch covers
packed wrapping/saturating arithmetic, comparisons, permutations, shifts, funnel
shifts, accumulator layout, scalar/packed products, floating conversion/division/
square-root, native exception returns, conditional links, traps, coherent-RAM
ordering, vector transfers and masked macro arithmetic/conversions. Boundary
inputs, source/destination aliases, register zero, flags, native ownership stops,
budgets and host rounding modes are checked together.

`analysis` covers discovery of both conditional-link continuations. The generator
fixture checks grouped missing-operation diagnostics and rejects ERET in a delay
slot before emitting output. `ee-llvm-project` retains independent generation,
compilation, execution and dependency checks. Shared numeric/vector changes also
affect `hardware-registers`; run that affected regression when changing the common
arithmetic. The combined Release run passed all five affected checks:
`ee-llvm-execution`, `ee-llvm-project`, `analysis`, `hardware-registers`, and
`repository-content`. Tests were deferred until after the requested 30-minute
implementation window; no integration run was repeated for individual additions.

The larger local GT4 conversion now passes the earlier MULT1 and unaligned-word
boundaries. Its grouped report identifies a CP0 Wired write and an indexed
cache-tag load (DXLTG), so it rejects conversion before creating a new project.
This is conversion progress, not compilation of that larger region, visible boot,
or gameplay.


## Native memory initialization milestone

The compiled EE batch now covers zero Wired reservations (including truncation of
upper source bits), rejection of nonzero reservations without retirement, Wired
and TagLo reads preserving upper GPR lanes, and indexed cache operations with no
RAM backing and an out-of-range index operand. The affected Release tests
`ee-llvm-execution` and `ee-llvm-project` both pass after this batch.

A new ignored local GT4 startup project successfully emitted LLVM and compiled
with Clang into a native executable. With a one-million-operation budget, it
retired 33,261 operations before stopping at unhandled syscall 64. This supersedes
the earlier Wired/DXLTG conversion boundary; it is not a visible game boot.


## Native semaphore milestone

The compiled-syscall lifecycle verifies creation through a RAM alias, count
acquisition/signaling, empty poll, nonretiring contended wait and overflow,
transactional protected/invalid status writes, returned status, deletion, table
exhaustion, handle reuse, invalid creation, and isolation between service owners.
The affected Release `ee-llvm-execution` and `ee-llvm-project` tests pass.

A fresh ignored GT4 startup project emits and compiles successfully. Its native
executable passes CreateSema and reaches SetSyscall (116) after 33,296 retired
operations with a one-million-operation budget. This supersedes the CreateSema
boundary above; visible boot is still unestablished.


## Compiled syscall-handler milestone

The native LLVM execution fixture registers original synthetic handlers, calls a
nested handler, and verifies eight retired operations and restored RA/SP/EXL with
both single-operation and combined budgets. It also covers aliases, exact compiled
membership (including a hole in a code extent), invalid alignment, unsupported
table aliases, removal and protected stack writes. The affected Release execution
and generated-project tests pass.

A freshly generated and compiled GT4 startup project still stops after 33,296
operations. The stop is now a rejected registration of syscall 131 because its
handler is not in the discovered instruction set, rather than an unimplemented
SetSyscall service. Callback discovery remains necessary; visible boot is unproven.


## Callback discovery milestone

Synthetic analysis covers table-loaded and pre-call callback arguments, delay-slot
loads, executable target validation, aliases, registered syscall numbers, wrong
wrapper numbers and unknown inputs. The affected Release `analysis` and
`ee-llvm-project` tests pass. The converter feeds callback roots and registered
syscall continuations back into bounded discovery before LLVM generation.

A fresh local GT4 project emits and compiles, registers the previously missing
handler and executes beyond its return. One-million and ten-million-operation
runs reach their budgets. A 100-million-operation run stops at 43,287,122 with a
read at 0x02000000, outside the 32 MiB RAM backing. This is a real memory boundary,
not a boot result or proof of correct handler behavior. No runtime instruction
decoder or compiler is added. The cause of the out-of-bounds read is unresolved.


## Native registration metadata milestone

The prior out-of-range scan came from a compiled handler searching kernel-address
RAM for its callback pointers, while native registrations existed only in host
storage. Project generation now reserves an ELF-disjoint metadata extent and
publishes actual registrations there. Handler lifecycle tests verify alias
publication, clearing on removal, and nonretiring rejection of altered metadata.
The affected Release execution and generated-project tests pass.

A freshly generated and compiled GT4 project completes both pointer searches and
stops after 198,305 operations at a read from 0x10001810 in the Timer 3 register range.
The prior 43-million-operation RAM overrun is no longer reached. The next native
vector-service boundary remains unsupported; no visible boot is established.


## Native polling timer milestone

Corrected the previous address classification: 0x10001810 is Timer 3 MODE.
Compiled LW/SW tests now exercise the native I/O boundary and timer owner with an
injected monotonic clock: start/stop, prescaling, wraparound, aliased addresses,
target masking, unsupported modes without retirement, unknown/misaligned accesses,
and missing providers. The affected Release `ee-llvm-execution` and
`ee-llvm-project` tests pass.

A freshly emitted and compiled GT4 project passes timer initialization and reaches
unhandled FlushCache (syscall 100) after 202,147 operations. Registered-handler
memory-copy calls precede it; copied content and executable-publication intent
still require analysis. No visible boot is established.


## Native cache-service milestone

Compiled syscall tests exercise all four modes through FlushCache and iFlushCache,
verify single retirement and preservation of the void-return register, and reject
invalid modes without mutation. They also verify that a completed flush neither
allows execution at an uncompiled RAM address nor permits its registration as a
handler. The affected Release execution and generated-project tests pass.

A freshly emitted and compiled GT4 project completes data and instruction-cache
flush calls. At 202,160 operations it attempts to register syscall 91 at the target
of an earlier copied payload. The target is not compiled, so registration stops
without mutation. Ahead-of-time payload discovery, relocation and publication
validation remain required. No copied instructions execute through a fallback.


## Ahead-of-time copied payload milestone

Synthetic compiled execution verifies absent and partial payload rejection,
execution after exact initialization, mutation detection across a budget pause,
retry after restoration, ordinary compiled initialization stores, and truncated
RAM backing. Project tests verify valid mapped compilation and reject non-file-
backed sources, overlapping destinations, malformed extents and invalid entry
offsets before output creation. Analysis tests cover materialized return-pointer
candidates. The affected Release execution, project and analysis tests pass.

Local commercial plans remain ignored. The first mapped GT4 payload compiled and
executed through syscall 91; its argument-dependent handler table then required
additional explicit entry offsets in the same private plan. Publication checking
uses whole-payload comparisons before each instruction and is not a performance
claim or support for arbitrary self-modifying code.


With all identified table entries supplied together, the local payload project
emits, compiles and executes through its syscall registrations. It stops at
202,348 operations on SetSyscall entry 0x12c, the existing project's Timer 3
interrupt-handler alias contract. Native interrupt ownership and delivery remain
unfinished. This result does not establish visible game boot.


## Native interrupt delivery milestone

Compiled synthetic handlers verify Timer 3 alias registration, full interrupted
context restoration, preserved retirement count, single-operation budget pauses,
masked events remaining pending, protected-stack rejection without acknowledgement,
and unhandled lines retaining their event. The affected Release execution and
generated-project tests pass. No timer interrupt is synthesized by this milestone;
tests explicitly raise native events.

The fresh local GT4 project emits, compiles and passes the Timer 3 alias and a
subsequent syscall-handler registration. It stops after 205,628 operations at
AddIntcHandler (syscall 16), requesting line 11. Public registration chains and
ordering remain the next native owner work. Visible boot is unestablished.


## Public INTC chain milestone

Compiled callback lifecycle tests verify head/tail/before-ID ordering, argument/GP
use, removal, ID reuse, table exhaustion, negative-return short circuiting,
single-operation budget pauses, idempotent enable/disable and invalid registration
inputs. The affected Release execution and generated-project tests pass.

The new local GT4 project emits, compiles and passes AddIntcHandler for line 11.
It stops after 205,718 operations on Timer 2 MODE at 0x10001010, writing 0x0f82
through a kernel RAM alias. Native compare/overflow event production is needed;
the polling timer owner rejects those modes without mutation. No visible boot is
established.


## Native timer-event milestone

Fake-clock execution tests now cover fractional-tick accumulation, exact
compare/overflow crossings, sticky status, write-one-to-clear rearming, repeat
suppression after INTC acknowledgement, stopped clocks, invalid modes and
backwards-clock handling. A compiled loop without I/O is interrupted through the
runner's bounded producer polls, executes a compiled callback, restores its
context and consumes exactly the requested operation budget. The affected Release
execution and generated-project tests pass.

The newly generated and compiled local GT4 project passes Timer 2 interrupt-mode
configuration and EnableIntc, then stops at CreateThread (syscall 32) after 205,828
operations. Timer event delivery is proven by the synthetic execution fixture;
this short GT4 run does not establish that its timer callback fired or that the
game booted. Native thread lifecycle and scheduling remain unfinished.


### LLVM native thread lifecycle and entry discovery

The compiled execution milestone creates and starts a worker, checks status and
invalid deletion, lowers the main thread priority, executes the worker's memory
write and ExitThread, then verifies restoration of the main register context and
shared CPU control state. It also checks exact retired-operation accounting,
dormant ID reuse, protected status output, malformed descriptors and uncompiled
entries. Analysis fixtures cover initialized descriptors, aliased entries, local
stack stores, stack-pointer moves, stores after argument setup, dynamic values,
nonexecutable/unaligned targets and wrong syscall wrappers.

Release `ee-llvm-execution`, `ee-llvm-project` and `analysis` passed for the combined
milestone. The additional descriptor-pattern correction passed `analysis` again.

The private GT4 project was regenerated and compiled with the existing explicit
copied-payload plan. Native execution passed CreateThread, GetThreadId and
ChangeThreadPriority, then stopped at GetOsdConfigParam (syscall 75) after 205,894
retired operations. This proves additional startup progress, not visible boot or
GT4 worker execution. All commercial inputs and generated artifacts remain
ignored local files.


### LLVM application settings milestone

Compiled syscall execution covers both primary and extended settings reads/writes,
constructor-supplied values, partial byte updates, instance isolation, default
values, void return-register preservation and exact retirement. Invalid alignment,
truncated buffers, out-of-range slices and protected output must leave state
unchanged; an empty extended slice performs no memory access. Release
`ee-llvm-execution` and `ee-llvm-project` passed together.

The private GT4 run passed its primary settings reads/writes and reached another
copied handler registration after 207,620 operations. A separate ignored mapping
plan was extended with the copy's source, destination and extent; no commercial
addresses or instruction data were added to implementation or synthetic tests.

The mapped payload project emitted LLVM, compiled and executed its first handler;
it stopped at another uncompiled entry registration after 207,652 operations.
Adding further private entry candidates exposed unsupported CP0 writes during
conversion. Inspection identifies address-translation registers and TLB operations
in that region. The expanded conversion fails before producing an executable;
no successful TLB behavior or visible boot is claimed.


### Compiled address-space request boundary

An original compiled fixture covers the five translation parameter selectors and
four mapping operations as a family. It verifies low-word write arguments,
sign-extended read results with upper lanes preserved, no retirement or destination
mutation on failure, retry at the same PC, and absent-provider stops. The recorder
is a test of the native request interface, not an address-translation implementation.
Release `ee-llvm-project` passed; after adding explicit fixture continuation
boundaries, `ee-llvm-execution` passed. The expanded private payload plan now emits
LLVM successfully.

The expanded GT4 project generated, compiled and ran through further copied-handler
registrations to 3,802,421 retired operations. It stopped on the unhandled DMA
controller status read at 0x1000e010. No address-space request was executed on this
run, so it establishes neither mapping-provider behavior nor visible game boot.
The generated target still links only LLVM output and native service/support
sources; it has no runtime decoder, interpreter or JIT fallback.


### Native transfer completion milestone

Compiled loads/stores exercise the DMA status view through an aliased address.
Coverage includes explicit multi-channel completion, invalid producer channels,
mask toggles, selective acknowledgement, unrelated interrupt-class preservation,
sign-extended status reads and independent owners. An enabled event stops compiled
execution with no retirement when a DMA handler is unavailable, retaining the
pending completion. The execution test passed. The existing private LLVM project
was relinked against the native completion owner and progressed to the SIF0
channel-control access at 0x1000c000 after 3,802,426 operations. Visible boot remains
unestablished.


### Native SIF receive and coordination milestone

Compiled I/O configures reception; synthetic native packets verify complete copies,
normal versus chain destinations, nonterminal delivery, tag-interrupt termination,
completion publication and explicit stop. Protected/out-of-range destinations,
active extent edits, duplicate delivery and unsupported modes must fail without
partial updates. Compiled syscalls cover initialize/reinitialize rejection,
stop/rearm, void return preservation, local readiness, absent peer readiness,
explicit peer publication, flag acknowledgement and sign-extended software slots.

Release `ee-llvm-execution` and `ee-llvm-project` passed together. The private GT4
project was regenerated, compiled and run; it passed SifSetDChain and stopped at
AddDmacHandler (syscall 18) for SIF0/channel 5 after 3,802,442 retired operations.
This establishes receiver initialization, not a connected peer, a GT4 packet
delivery, visible boot or gameplay. Synthetic packets prove receive behavior.


### Native packet-to-DMA-handler milestone

Compiled syscall registration installs DMA and INTC callbacks with independent
handle IDs. A synthetic native packet completes SIF0 reception; simultaneous INTC
readiness verifies DMA priority and preservation of the other domain. A protected
stack blocks delivery without acknowledging completion. A budget pause inside the
compiled callback resumes, restores PC/registers/stack, and preserves exact retired
counts before the pending INTC callback runs. Removal, idempotent enable, negative
aliases and invalid channel IDs are covered in the same lifecycle.

The new test initially crashed with a large local RAM fixture; moving that fixture
to heap storage resolved the failure. The affected compiled execution test then
passed. The project-generation test also passed.

The private GT4 project was regenerated and compiled. A traced run passed DMA
registration/enablement and repeatedly called SifGetReg for peer readiness; it was
stopped to avoid redundant trace growth. A separate untraced run consumed exactly
10,000,000 operations and ended on budget in that polling loop with zero peer flags.
This is an unmet native-peer dependency, not visible boot or evidence of GT4 DMA
callback execution. The synthetic integration test proves callback delivery.


### Native outgoing SIF batch milestone

Compiled SifSetDma/SifDmaStat calls exercise an explicit native sink. A malformed
second descriptor must not publish the first. A valid padded two-transfer batch
retains its submitted bytes despite subsequent descriptor/source mutations. The
sink applies all transfers together after backpressure, and a retryable failure
preserves the snapshot without instruction retirement or completion. Acceptance
raises channel 6 exactly once; status and busy submission behavior are checked.
Missing sinks are rejected and peer readiness is never fabricated. Production IOP
connection and GT4 readiness remain outstanding.

Release compiled execution and project-generation tests passed; execution was
rerun successfully after adding fault-address reporting. No new GT4 run is claimed
for this transport interface alone.


### Native bootstrap command peer milestone

Compiled EE services query a genuinely connected native command sink, submit
command-buffer and software-register packets, and observe completion after the
peer applies them. The fixture checks the aliased receive address, software value,
readiness/address publication and rejection of duplicate connection. A reboot
packet is rejected without instruction retirement, completion or loss of the
pending transfer. Release compiled execution and project-generation tests passed.

The private GT4 project regenerated and compiled successfully. Native execution
left the readiness polling loop, submitted command-buffer initialization, and
stopped on command 0x80000002 option 1 (RPC initialization) after 3,802,917 retired
operations. A second untraced run confirmed that command and option. RPC readiness
was not fabricated. Visible boot and gameplay remain unestablished.


### RPC control reply and launch-context milestone

Compiled tests cover command-buffer setup, RPC initialization backpressure,
failed reply-buffer validation with retry, reply visibility before DMA delivery,
launch context plus compiled EI, callback entry/return, and an unavailable-server
bind reply with preserved correlation fields. EE and IOP compiled execution tests
passed after sharing the bind-reply encoder. Large EE fixture RAM buffers were
moved to heap storage after a test crash; the rebuilt tests passed.

The private GT4 executable was relinked and executed with the explicit native
launch context. It consumed the RPC reply interrupt and reached its compiled SIF0
handler, stopping at a load from 0x2104a580 after 3,802,938 operations. Address
resolution for that access remains incomplete. No visible boot is established.


### Native RAM alias and SIF rearm milestone

Compiled loads/stores cover both application uncached windows, their first mapped
address, reserved lower boundary, upper boundary, actual backing extent, alignment
and compiled-code write protection. Native settings and SIF delivery use the same
aliases. The RPC lifecycle fixture now executes negative SifSetDChain from inside
a compiled DMA handler and verifies reception is rearmed after context restoration;
outgoing batch tests cover negative submit/status calls.

Release compiled EE execution, project generation and hardware-register tests
passed for alias changes. After the SIF alias additions, the compiled execution
and project tests were run again. An attempted obsolete native-execution build
target referenced the removed C++ backend; it is not a current CMake test target
and was replaced with the current shared-runtime hardware test target.

The private GT4 project was regenerated and compiled after both changes. It
passed the uncached reply-buffer access and interrupt-safe receive rearm, then
stopped at an uncompiled indirect command callback after 3,802,999 operations.
This is a discovery boundary; no runtime decoding fallback was added. Visible
boot remains unestablished.


Stored callback discovery milestone:

- `analysis` passes original synthetic table stores, direct-call delay-slot
  stores, invalid/non-executable targets, unknown values and volatile-register
  boundaries.
- `ee-llvm-project` now stores a function pointer into writable data, loads it
  and calls it indirectly. The converter must discover that target before LLVM
  compilation; the resulting native executable returns the expected result.
- Private GT4 conversion and native compilation succeed. A bounded native run
  reaches a second undiscovered registered RPC callback after 3,803,412 operations,
  beyond the previous command-table callback. No game boot is claimed.


Registration-function discovery milestone:

- `analysis` covers callback arguments copied in branch delay slots, modified
  pointers, unknown values, non-executable data, branch-likely annulment and loops.
- `ee-llvm-project` now passes a callback pointer to an original registration
  function, then loads and calls the stored pointer in the compiled executable.
- `ee-llvm-execution` covers the negative interrupt-safe semaphore aliases in
  the existing compiled service lifecycle, including status-buffer validation
  and preservation of interrupt state.

- Regenerated private GT4 LLVM output compiles and executes the RPC completion
  callback and interrupt-safe semaphore signal. A 100,000,000-operation bounded
  run records 14 completion signals while retrying the unavailable file-I/O
  service. This confirms callback return and repeated delivery, not game boot.


Native file-I/O RPC milestone:

- `ee-llvm-execution` includes an original synthetic ISO and full native RPC
  lifecycle: bind, backpressure/retry, open, unaligned read, EOF, seek, version,
  close, invalid descriptor, protected output, immutable request snapshot and
  malformed payload. Replies run through a compiled interrupt callback and rearm
  reception. Failed preflight checks do not consume a file handle or cursor.
- `ee-llvm-project` compiles the generated project with direct native ISO/file
  sources and retains the check excluding runtime execution scaffolding.
- `disc-fileio-rpc` passes for the existing shared service integration.
- Private GT4 generation/build succeeds; execution with the explicit ISO completes
  one FILEIO request and stops at another undiscovered indirect target after
  3,805,328 operations. Actual GT4 file opens/bytes read remain zero.


ELF lifecycle discovery milestone:

- `formats` validates typed initialization arrays, legacy constructor/destructor
  metadata, aliases and count/sentinel forms; malformed layouts, names, extents
  and non-executable targets are rejected.
- `ee-llvm-project` compiles and executes an original program which loads a
  constructor from its ELF table and calls it indirectly, checking the changed
  result and exact retired-operation count.
- `analysis` continues to pass the existing callback/control-flow milestone.

- Private GT4 conversion and native compilation succeed with lifecycle roots.
  Execution proceeds past the constructor table and stops on unsupported
  scratchpad storage after 3,807,087 operations. No game boot is claimed.


Native scratchpad storage milestone:

- `ee-llvm-execution` compiles byte, word, doubleword and quadword stores/loads
  against independent scratchpad storage, including the last valid extent.
  Checks cover unchanged main RAM, distinct per-platform storage, an unaligned
  word, out-of-range access, missing backing storage and a truncated buffer.
- `ee-llvm-project` exercises the generated project's memory ABI and complete
  generation/build/execution pipeline with the updated lowering.

- Private GT4 generation, native compilation and execution succeed through
  scratchpad initialization. The run stops at unsupported GS CSR access after
  3,808,495 operations; this is startup progress, not rendering or game boot.


Native graphics lifecycle milestone:

- `ee-llvm-execution` exercises compiled 64-bit service stores/loads, full-width
  values, failed-read state preservation, misalignment, missing/failed reset
  backends, successful reset, CSR/mask reads and flush without an extra reset.
- `ee-llvm-project` builds and executes converted fixtures with the shared native
  Vulkan target while continuing to reject diagnostic execution dependencies.
- `vulkan-rendering` synchronizes the actual device before and between existing
  sprite rendering comparisons, retaining its validation-error checks.

- The graphics lifecycle test also executes GsGetIMR/GsPutIMR and their negative
  aliases through compiled syscall boundaries, checking mask consistency and
  preservation of the void setter's return register.

- The private generated executable creates a real Vulkan renderer on the local
  GPU, completes one reset and mask configuration, then reaches unsupported
  SetGsCrt after 5,968,757 operations. This verifies initialization, not a rendered
  GT4 frame, visible boot or gameplay.


Native SetGsCrt/display-clock milestone:

- `ee-llvm-execution` covers compiled SetGsCrt calls, invalid modes, synchronization
  failure preserving configuration, NTSC/PAL boundaries, CSR acknowledgement,
  interrupt unmasking, field parity, rollback, large elapsed intervals and reset.
- An original compiled VBlank handler receives the event and returns through the
  native interrupt dispatcher; timing comes from an injected clock.
- `ee-llvm-project` exercises conversion/build/execution with the updated mode
  service and memory interface.

- Private GT4 generation, compilation and execution pass SetGsCrt on the native
  Vulkan-backed path. Execution reaches unsupported VIF0 reset/control access
  after 5,968,955 operations. Visible boot and rendering remain unverified.


Native VIF/GIF initialization milestone:

- `ee-llvm-execution` covers compiled reads/writes for both VIF channels, reset,
  stop/break/cancel, masked configuration, channel isolation, GIF pause/reset,
  and explicit rejection of FIFO input without retirement or completion.
- `ee-llvm-project` passes generated LLVM compilation and execution.
- The private GT4 executable passes VIF0 reset and stops at an unsupported VU
  control read after 5,968,957 operations. This is startup progress, not boot.


Native vector-owner milestone:

- Compiled CTC2/CFC2 transfers use the production platform owner, including masked
  control readback, active-work backpressure, retry and idle reset.
- The same milestone checks shared status/MAC storage, integer widths, read-only
  controls, constant integer zero and unsupported selectors.
- Private GT4 execution passes vector-control setup and reaches VIF0 DMA access
  after 5,969,004 operations. No boot or rendered frame is claimed.


Native packet-submission milestone:

- Compiled SW/LW accesses configure all three channels and verify consumer
  delivery, source/count updates and completion only after successful consumption.
- Missing/rejecting consumers, unsupported chains, unaligned/out-of-range sources
  and scratchpad boundaries reject without consuming a packet or raising completion.
- Synthetic consumers verify the interface; they do not establish a production
  VIF/GIF packet implementation. The private GT4 run stops at global DMA priority
  access after 5,969,128 operations, before packet submission.


Submission-policy coverage uses compiled accesses to verify cold control state,
enable/suspend readback, disabled and suspended starts without consumer calls,
rejected arbitration/mode changes without mutation, rejecting-consumer retry,
and zero-only channel initialization. The private rebuilt executable reaches a
VIF0 chain submission after 6,363,631 operations with submission enabled; no packet
consumption, boot or game frame is established by this result.


Native chain/configuration milestone:

- Synthetic compiled submissions cover REFE/CNT/NEXT/REF/END, upper-tag commands,
  payload commands, tag interrupt termination and successful native configuration.
- Cyclic chains, invalid source extents and unsupported descriptors fail without
  completion. A later unsupported MPG command rolls back an earlier MARK in the
  same batch; truncated row configuration also preserves prior configuration.
- Private GT4 execution identifies MPG (0x4a) as the first unsupported command in
  the resolved chain, at 6,363,631 operations. No VU upload, launch or game frame is
  reported as successful.


VU input milestone:

- `vu-program-framing` uses original synthetic words to test pair order, immediate
  versus lower-instruction selection, flags, operands and VU0/VU1 extent validation.
- The compiled packet milestone checks that complete MPG input reaches the optional
  observer, truncated input does not, and neither case commits packet configuration.
- `ee-llvm-project` continues to compile and execute generated code independently
  of the offline VU library. Framing tests do not validate VU execution semantics.
- The opt-in private capture was inspected with ps2-vu-inspect. The capture remains
  under ignored work/ and is not a repository fixture.


The VU frontend milestone also covers synthetic conditional/direct/call/return
control flow, separate delay-slot visitation, E-bit termination, signed target
wrapping, loops, missing slots, branch-like immediate data and duplicate-address
rejection. Branches inside delay slots are reported as unresolved complex ordering.
Only `vu-program-framing` is required for this offline-only change; no generated
runtime behavior changed. Private inspection is a candidate inventory, not a boot
or native VU execution test.


Upper arithmetic frontend coverage extends the VU milestone with original synthetic
encodings for vector destinations, masked operands, all broadcast lanes, accumulator
forms, I/Q selection, conversion scales and Ft destinations, outer products, ABS,
CLIP, NOP and reserved cases. These tests verify typed decoding only; they do not
compare numerical execution or timing. Only the offline VU tests are rerun for this
change because generated runtime dependencies are unchanged.


Lower frontend tests extend the VU milestone across integer register/result
selection, signed five-bit and unsigned fifteen-bit immediates, vector masks,
rotation, scalar lane selection, I/Q destinations, WAITQ dependencies, register-bank
transfers and shared branch decoding. Reserved encodings stay unsupported. All
fixtures are original synthetic encodings; numeric execution remains untested
because this stage only constructs typed conversion records.


`vu-llvm-execution` compiles original synthetic VU pairs to LLVM, compiles the IR
with clang and links only the generated object plus a C++ state-checking driver.
It exercises integer wrapping, same-pair old-vector reads, in-place rotation,
signed integer broadcasting, constant VF0, I literal publication and E delay-pair
execution. Its fixture emitter also verifies that an unsupported arithmetic family
fails without partial LLVM output. No interpreter or frontend library is linked
into the execution test. Numeric/branch/Q families remain outside this milestone.


The compiled VU arithmetic milestone now exercises ITOF4/FTOI4 across signed
values and conversion saturation, bit-ordered min/max including signed zero and
exponent-255 inputs, a masked broadcast with preserved destination lanes, and an
upper I operand paired with a lower replacement literal. It links the existing
numeric helper source rather than an interpreter or a second numeric implementation.
Only vu-llvm-execution and repository content checks are needed for this lowering
extension; shared helper implementation and unrelated runtime code are unchanged.


Finite VU native arithmetic extends vu-llvm-execution with ADD, masked broadcast
SUB, masked accumulator MUL with I, and MUL to constant VF0. Checks cover results,
preserved accumulator lanes, zero/sign/underflow MAC flags and sticky status.
Nonfinite and overflowing operands must reject the whole native region without
changing any input State fields, even after earlier pairs would have changed them.
The EE LLVM execution milestone also passes after factoring the shared flag policy.
These checks establish finite arithmetic and atomic failure, not branch/Q pipelines
or GT4 upload execution.


The VU native milestone additionally executes masked MADD/MSUB vector results,
broadcast MADDA and immediate MSUBA accumulator updates. A cancellation fixture
checks separate product rounding rather than fused arithmetic. Intermediate
product underflow is rejected both by the shared helper and by a compiled region,
with every input State field unchanged. The EE native milestone also passes with
the extended shared numeric source.


The VU compiled milestone now includes separate conditional and indirect-call
objects. It verifies taken/untaken paths, unconditional delay-pair execution,
previous integer writes seen by branch operands, incoming delayed operands across
block boundaries, same-register JALR target/link ordering and atomic rollback when
upper arithmetic fails in a branch pair. The native driver links generated objects,
not a decoder. Private first-block LLVM generation and x64 object compilation also
succeeded, without executing the commercial block or claiming boot progress.
# Native VU routing milestone

`vu-llvm-execution` compiles original branch and E-bit continuation fixtures through
LLVM and executes them through the native block registry. It covers both branch
outcomes, delay-slot effects, bounded pause/resume, zero budget, missing indirect
targets, numeric failure rollback and malformed registries. This executable links
the generated objects and native numeric helpers, without the offline decoder or
an instruction interpreter. These synthetic results do not establish game boot.

The quotient milestone extends this same compiled execution test with a DIV result
carried across a branch, paired WAITQ/MULq, SQRT and RSQRT, pending-state resumption,
numeric and schedule rollback, invalid/sticky status and signed zero exceptions.
The shared native numeric translation unit also builds and passes `ee-llvm-execution`.
The private whole-upload conversion probe now reaches the single-block-boundary
diagnostic rather than the missing-Q-family diagnostic. This does not execute the
upload or establish GT4 boot progress.

The multi-block milestone generates one LLVM module from an original conditional
program, compiles its native functions and table, and executes both branch outcomes
using the generated table through `Program`. It checks delay-slot side effects and
completion counts. Generation also rejects a late unsupported block, a missing
delay pair and complex delayed control flow without emitting partial output.
Private candidate-block compilation is separate evidence; no commercial code or
capture is a committed fixture, and compilation does not establish execution.

Upload extraction extends `vu-program-framing` with multiple MPG chunks, count-zero
encoding, payload words resembling commands, launch boundaries, truncated packets,
unit limits, interrupts, overlapping writes and sparse image assembly. The native
packet integration test checks the full-packet observer receives the rejected batch
without committing MARK, publishing completion or observing truncated MPG data.
The private capture/extraction run confirms two adjacent chunks; it does not accept
or execute their upload in the runtime.

The quadword-memory milestone compiles original load/store programs for both VU
units. Native execution checks masked stores, pre-decrement/post-increment, VF0
constant stores, ignored loads to VF0, signed displacements, the differing memory
wrap sizes and whole-block rollback after a late VU0 register-window access. The
private 30-block object is compilation evidence only; it is not a test fixture.

The binding milestone links a generated module's identity accessors and native
table into `vu-llvm-execution`. It verifies that partial coverage cannot launch,
wrong content or unit cannot commit, a later unsupported packet command prevents
binding, a complete matching upload commits configuration and launches native code,
and reset blocks resumption. The affected EE packet regression also passes. The
native test links no instruction decoder, offline frontend, or interpreter.

Generated-project integration passes `ee-llvm-project` with the optional module
disabled and the native binding milestone with the shared packet adapter. The
opt-in private regenerated project links its 30-block VU0 module, verifies the full
upload identity, and stops on the next VU1 upload. Its generated build source list
contains native services and LLVM objects, not an interpreter or offline decoder.
This establishes startup upload progress only, not VU execution or visible boot.

The integer-memory milestone extends both native unit fixtures with ILW/ISW and
ILWR/ISWR, checking zero extension, lane masks and last-selected-lane priority.
A separate generated load/branch block checks low-16-bit truncation and immediate
branch visibility. Both VU milestone tests pass. Private inventory counts describe
decoder coverage only; the next unlowered VU1 block remains an explicit stop.

The launch-input/outer-product milestone compiles XTOP, XITOP, OPMULA and OPMSUB
together. It verifies supplied offsets, missing-input rollback, xyz cross-product
results, preserved w values and flags, and unsupported numeric rollback. Both VU
milestone tests pass. Private conversion advances to an extended-function gap at
pair 533; no execution or boot claim follows from that diagnostic.

The P-register milestone generates a multi-block VU1 module with ERLENG, a branch,
WAITP and masked MFP, plus an unsynchronized read entry. Native execution checks
pending-state transfer, reciprocal length, zero length, unaffected flags, numeric
rollback and schedule rollback. Related VU tests and shared EE numeric execution
pass. VU fixture compilation now uses one reusable CMake loop instead of repeated
per-fixture compiler commands.

The MAC-read milestone generates an arithmetic writer, branch, settling pair and
FMAND/FMOR/FMEQ readers, with an alternate early-read entry. Native execution
checks guard preservation across blocks, correct masks/comparisons, low-16-bit
flag selection and atomic rejection of unproven visibility. Both VU milestone
tests pass. Private conversion now stops on XGKICK rather than the MAC-read family.

The graphics-boundary milestone generates a VU1 program split at XGKICK and verifies
pending requests prevent continuation. A synthetic accepting consumer receives an
immutable wrapped packet before the continuation modifies its source data. Missing
or rejecting consumers and malformed packets preserve the pending request. Packet
tests cover multi-tag register-list/image framing and missing end tags. This is a
native interface test, not Vulkan rendering or asynchronous PS2 transfer validation.

The combined graphics/E milestone compiles a kick with an integer-modifying delay
pair. It verifies source-address capture, exactly-once delay execution, rejection
without completion, acceptance followed by completion without another native block,
and stable repeated completion checks. A delay-pair data store is rejected before
module emission. Both VU milestone tests pass; private conversion reaches clip flags.

The clip milestone compiles CLIP with FCAND/FCOR/FCEQ/FCSET/FCGET across a branch.
Native execution checks history rollover, masks, comparisons, low-twelve-bit reads,
unchanged MAC/status, four-pair visibility and early-read rollback. Numeric edge
checks cover signed zero, equal thresholds and denormals. Related VU and shared EE
tests pass. The private VU1 module compiles to a 392-block x86-64 object, without
claiming that those blocks have executed or that all possible targets were found.

The dual-unit binding milestone compiles separate synthetic VU0/VU1 modules and
links both into the execution test. It checks independent partial upload coverage,
duplicate rejection, failed-upload isolation and native execution of the VU1
artifact. Related Release gates are `vu-llvm-execution`, `vu-program-framing` and
`ee-llvm-project`. Private startup confirms both uploads bind but stops before
game VU execution; this is not visible boot evidence.

The native metric boundary is covered by `ee-llvm-execution`: generated code
performs control and both counter transfers through a synthetic provider, passes
execution mode, preserves signed readback, and leaves state and retirement
unchanged after rejection. Existing disabled-counter and absent-provider cases
remain covered. `ee-llvm-project` verifies linkage of the emitted calls through
the generated build. The synthetic provider does not establish a production
measurement source or console timing equivalence.

The VIF1 launch milestone in `vu-llvm-execution` submits configuration and MSCAL
packets to an actual LLVM-compiled synthetic program. It checks native results,
TOP/ITOP inputs, alternating buffers, rejection of trailing commands, missing
entry rollback, bounded-execution rollback and rejection of VU0 without shared
state ownership. `ee-llvm-execution` covers affected packet integrations.

The V4-32 milestone uploads vectors and launches an LLVM-compiled load program in
one submission. It checks wraparound, TOPS-relative writes and atomic rollback for
truncated input, unknown trailing commands, unsupported cycles and missing native
entries. This proves synthetic native data consumption, not game rendering.

The packed V4 milestone extends that same compiled-load integration to signed and
unsigned 16-bit/8-bit lanes and packed 5:5:5:1 colour. Cases cover negative limits,
nonzero padding, packed-colour MODE independence, 256-vector encoded-zero counts,
masked-format rejection and truncated payload rollback. Related Release gates
are `vu-llvm-execution` and `ee-llvm-execution`.

The shared-state milestone compiles a VU0 program that adds vector lanes and
increments an integer register. Packet launches verify EE-to-VU input visibility,
VU-to-EE vector and flag publication, integer wraparound, repeated launches using
new macro state, ITOP metadata, active-owner rejection and missing-entry rollback.
It runs with both VU submission owners registered and uses the same related
Release gates. This is synthetic coverage, not a claim of game VU execution.

The VCALL milestone links generated EE and VU objects into the same execution
test. It verifies immediate launch, CMSAR0 address masking, missing binding and
entry failures without retirement, and a pending launch inside an EE branch delay
slot followed by exactly-once retry and continuation. Both native execution gates
and generated-project integration cover the affected call ABI.

The vector-memory milestone executes compiled EE quadword, word and byte loads
and stores against both native VU owners, VU0 mirrors and a kernel alias. It then
runs compiled VU code against EE-written data. It checks signed readback, active
ownership rejection without retirement, supplied-buffer bounds and rejection of
instruction-window writes. The related gates are the two native execution tests
and generated-project integration.

The graphics-batch milestone compiles a program with two XGKICK boundaries and
a data write between them. The native launch test verifies ordered immutable
snapshots, a single whole-batch acceptance call, rejection without state commit,
and no delivery when the shared execution budget expires. The affected gate is
`vu-llvm-execution`; no Vulkan rendering claim follows from this consumer test.

`vu-vulkan-rendering` closes the synthetic compiled-VU-to-GPU path: it binds and
executes an LLVM-generated XGKICK/E program, accepts its native graphics batch,
and verifies Vulkan pixels, exactly-once completion and zero validation errors.
`vulkan-rendering` covers packed/address-data and register-list sprite packets,
pixel coverage, and atomic rejection of malformed/truncated batches. The shared
sprite planner is also covered by `hardware-registers`. GPU gates require a
working Vulkan device and validation layers; none establishes game boot.

The direct-submission milestone extends `vu-vulkan-rendering` to VIF1 DIRECT,
DIRECTHL and direct GIF batches using the shared renderer. It verifies pixels,
paused-GIF rejection, encoded-zero transfer truncation and atomic configuration/
pixel rollback after an unsupported trailing command. Related packet integrations
remain covered by the EE/VU execution and generated-project gates.

The image-upload milestone adds RGBA32 upload/draw ordering and RGB24 continuation
across packets to `vulkan-rendering`. It checks destination alpha preservation,
ignored final-beat padding, failed-continuation rollback and retry, and rejection
after upload completion. `vu-vulkan-rendering` covers the connected consumer and
`hardware-registers` covers the shared pixel-address helper.

The native textured-sprite milestone uploads an original RGBA texture through GIF
image packets, configures UV sampling through drawing packets, and verifies the
Vulkan destination pixels. Perspective and palette requests must reject without
pixel changes. Existing Vulkan sampler and diagnostic graphics tests cover the
shared texture planner after extraction; the connected VU graphics test also runs.

The indexed-sprite milestone extends `vulkan-rendering` with original image and
palette uploads followed by PSMT4 sampling. It checks bank selection, conditional
snapshot retention, reload after changed source data, rejected-batch rollback,
and unsupported palette-format rejection. The shared planner is also exercised
by `hardware-registers` (both palette layouts and remembered bases); the compiled
VU integration remains covered by `vu-vulkan-rendering`.

The offline metric-dependency milestone is covered by the Release `analysis`
gate: synthetic ELF instructions propagate a counter value through arithmetic
into a store, identify a known destination, report a dependent load address,
and stop at return/delay-slot or unknown-operation boundaries. Tests also cover
the analysis budget and malformed segment extents. These are analysis tests,
not evidence that native performance measurements or game startup are complete.

Metric memory-reference analysis extends the same `analysis` gate with constants
established before measurement, overlapping byte consumers of stored words,
constant invalidation after returns and at direct targets, and malformed later
segments. Candidate-address reporting remains separate from proof of dead data.

Pointer-handoff coverage uses a synthetic shared structure with a counter field:
a call receives the base through its delay slot, a later call must not inherit
stale constants, and separate paths store and return the base pointer. These
cases run together in the existing `analysis` milestone gate.

The PSMT4 upload milestone runs `vulkan-rendering`, `vu-vulkan-rendering` and
`hardware-registers` in Release. Original one-index uploads test independently
specified block/page addresses, neighbouring-nibble preservation, and Vulkan
palette sampling of each uploaded point. A 33-index upload verifies continuation,
odd final-nibble padding, rejected-batch rollback and successful retry.

The native pixel-pipeline milestone runs the three graphics integration gates in
Release. Synthetic packet batches verify alpha comparisons and failure actions,
destination-alpha selection, source and fixed-alpha blending, negative rounding,
clamp/wrap, per-pixel blend selection and rejection without published pixels.
Existing texture and compiled-VU tests exercise the expanded Vulkan draw ABI.

The drawing-reset milestone uses `ee-llvm-execution` for callback rejection and
retry without premature instruction retirement. `vu-vulkan-rendering` verifies
retained image storage, rejection of stale upload/draw requests, and rendering
from compiled VU code after reconfiguration. `vulkan-rendering` additionally
checks 24-bit destination-alpha rules and atomic rejection of aliased blending.
Generated-project integration verifies the production lifecycle wiring builds.

The scalar/V2 conversion milestone runs `vu-llvm-execution` and
`ee-llvm-execution` in Release. Compiled VU loads verify every supported shape and
component width with signed/unsigned boundaries, repeated lanes, odd packed
transport padding, wraparound and encoded-zero count. Truncated V2 and unsupported
V3/masked/packed shapes must leave state unchanged. Existing V4 coverage remains
in the same test batch.

The masked-upload milestone runs the EE/VU native execution gates and connected
VU-to-Vulkan integration. Compiled loads verify all four lane selections, cycle
restart, column-group saturation, packed-colour selection and rollback of data
and configuration after truncated input. Older unsupported-command fixtures use
an invalid format rather than a newly supported masked command.

The skipped-cycle/fixed-row milestone runs the EE and VU LLVM execution gates.
Compiled VU loads check destination wrap with preserved skipped slots, unsigned
row-add overflow, unchanged row configuration and masked replacement semantics.
An unsupported accumulating-row submission must roll back both data and cycle
configuration. Existing scalar, V2, V4 and masked cases run in the same batch.

The mutable-row milestone runs the EE/VU LLVM execution gates. A synthetic batch
combines accumulating and replacement modes across multiple uploads before a
compiled VU load, verifies row readback and masked non-updating lanes, and rejects
a missing compiled entry without publishing either rows or vector data. Existing
format, spacing, mask and padding cases run against the shared write planner.

The masked filling-cycle milestone runs EE/VU LLVM execution tests. A compiled
load verifies row and column fills, protected lanes, accumulation, wrapped output
and a following MARK command after the reduced payload. Encoded-zero output count
covers 256 outputs from 64 scalar inputs. Truncated payload and input-selected
fill lanes must roll back data, mask and row configuration.

The masked V3 milestone runs the EE/VU LLVM execution gates. Compiled loads verify
all three component widths, signed/unsigned boundaries, nonzero padding and row
accumulation with an explicit fourth-lane replacement. Tests reject unmasked W,
a later input-selected W mask group, and truncated V3 payload without state changes.

`ee-llvm-project` now verifies post-link map/audit artifacts for actual generated
executables before running them. A negative case injects diagnostic-library link
evidence and a stale success report, then requires the audit to reject and remove
the report. This supplements the existing generated build-configuration check;
it does not substitute for executing generated code or source/dependency review.


The `vu-llvm-execution` milestone also compiles and executes a multi-entry native
continuation fixture. It covers MSCNT before initialization, ordinary E-bit resume
addresses, TOP/ITOP updates, a rejected graphics consumer and retry, continuation
after an E-bit graphics stop, and rollback of a data upload when the next compiled
entry is missing. These fixtures contain original synthetic code only.


The `analysis` milestone covers measurement-pointer callee candidates with a
synthetic ELF: likely-branch annulment, distinct pointer values at a join, return
delay-slot reads, nested-call boundaries, unsupported operations, and bounded
changing-pointer loops. It tests offline analysis only; it does not claim game
booting or establish that measurements can be removed.

The same analysis milestone verifies that conditional pointer replacement cannot
leave a stale address, identical conditional outcomes retain a candidate, typed
memory footprints preserve quadword alignment, unresolved addresses are reported,
and explicit breaks terminate exploration.


On Windows, `host-metrics-capabilities` validates profile-source response parsing
with original synthetic records, multiple linked entries, empty responses,
truncated records, invalid offsets, absent final records and unterminated names.
The separate live `anyps2-host-metrics` probe verifies enumeration on the current
host. Neither parser success nor live enumeration demonstrates counter collection
or game booting.

The VU continuation execution fixture now supplies only its initial entry to the
compiler and requests two continuation rounds. Its compiled native execution
therefore checks offline entry recovery as well as MSCNT submission. Requesting
a third round beyond the synthetic image must fail with a missing-pair diagnostic
and no partial LLVM output.

The metric analysis milestone includes a parent-structure argument whose callee
reads a measured child field, a stored parent pointer with a resolved destination,
and an unknown loaded pointer that must not inherit invented ancestry.

The host metrics milestone also checks native thread interval attribution using
original synthetic samples: initial partial intervals, active snapshots, CPU
migration, exclusion of other threads, lost-event invalidation, discontinuous
switches, counter regression, ordering errors, overflow and missing counter data.
It validates the attribution contract, not live ETW capture or PS2 metric semantics.

Synthetic Windows event records now exercise decoding through thread attribution,
including processor indices above 255 and ownership after source-buffer mutation.
The milestone rejects unsupported versions, truncated payloads, missing counters,
wrong counter counts and duplicate PMC extensions, and ignores unrelated events.

The same synthetic decoding/attribution scenario runs for context-switch V2
(24 bytes) and V5 (28 bytes). It checks exact version-specific lengths, rejects
unknown versions, and keeps nonzero unused V5 payload data out of thread/counter
results. Live V5 capture remains a separate elevated integration check.

The generated EE project milestone executes its original synthetic LLVM fixture
with `--capture-stop`, checks the stopped PC and result register plus RAM and
scratchpad extents, and verifies that a second capture cannot overwrite the first.
It retains the existing post-link dependency audit. Captures are diagnostic
observations and are not tested or advertised as resumable save states.

The EE execution milestone covers counter configuration/readback, unavailable
measurements, disabling without inventing a result, and explicit reset recovery.
The project milestone also compiles and executes an original object-table call
with a delayed argument adjustment and an otherwise undiscovered target. Analysis
checks malformed targets, a clobbered table base and unrelated object fields.

The same project milestone compiles and executes a synthetic bounded indexed
switch, selecting a non-first table target. Analysis rejects partial malformed
tables and bounds over an unrelated register.

The Vulkan transfer milestone splits an original image packet after its image
tag, verifies no early pixel publication, rejects completion and retries it,
checks swizzled destination pixels, and covers reset, input bounds and rejected
cross-path interleaving. The generated EE project milestone also reaches an
otherwise undiscovered SetupThread return trampoline through compiled code.

The thread milestone compiles two original EE programs which create/start a
worker, sleep the main continuation, and resume it through ordinary or
interrupt-safe wakeup. The interrupt-safe variant returns through the compiled
thread exit trampoline before the main continuation resumes. A worker-written
marker distinguishes immediate preemption from deferred switching. Native scheduler
checks cover credits, cancellation, dormant/invalid targets, wait status and
unchanged state when sleep has no runnable replacement.


The Release `analysis` milestone also covers nested measurement-structure calls,
call/return delay-slot effects, overlapping JALR target/link registers, unknown
callee argument replacement and bounded recursion. These original synthetic
fixtures validate offline candidate discovery; they do not establish native
counter equivalence or game boot acceptance. The same milestone covers scalar
and packed bitwise/copy propagation, integer-pointer preservation across
floating/vector arithmetic, and invalidation on upper-lane/coprocessor reads.


The Release `ee-llvm-execution` RPC milestone covers CD/DVD initialization with
an original synthetic ISO: binding, completion through a compiled interrupt
handler, initialization/reinitialization/shutdown, legacy version fields,
malformed output bounds, executable protection, unsupported requests, missing
media and a volume with a truncated root extent. The same milestone covers
ReadClock BCD/JST conversion, media-mode selection for CD and DVD, and
disk-ready results for an open session and a closed session. Rejected media
values, protected writes, and mode cleared by shutdown keep the previous
session state. Failed calls preserve session ownership and output bytes. The
tests share the FILEIO transport harness.

The EE semaphore scheduler regression executes a compiled WaitSema and verifies
that an idle wait retires it once, retains its continuation, and exposes the
actual waiter count. FIFO handoff, independent SleepThread wake credits,
interrupt-safe signalling without a duplicate token, and rejection of deletion
with live waiters are covered. The IOP LLVM transfer test accepts a word-aligned
source, rejects byte-misaligned sources without partial writes, and checks rejected
descriptor diagnostics. Analysis tests cover all eight EE argument registers,
callee-preserved callback copies across helper calls, and volatile/non-argument
rejection. Bounded switch coverage includes table addresses split between the
range-branch delay slot and an explicit low-half addition. These synthetic checks do not establish title-screen or gameplay support.

Persistent IOP worker tests execute a sleeping and waking pair with one-operation
and whole-run budgets, retire both continuations, and reach an actual idle wait.
Unknown executable targets still stop. Module-startup completion reporting is
unchanged. Generated EE/IOP project tests use the explicit --headless option so
Vulkan inspection windows cannot stall automation.

### Warning-thread continuation milestone

Related Release analysis and generated-project tests cover callback discovery
across independent word argument loads. Compiled EE execution checks semaphore
counts above the declared maximum, host cancellation, idle interrupt identity,
real wakeup of a sleeping continuation, interrupt activity without foreground
progress, and ExitDeleteThread handoff with handle reuse. Private GT4 execution
now completes the warning thread and reaches system-bus handler removal in the
main thread. This is startup progress, not title-screen or gameplay acceptance.

### Native cleanup and byte-output milestone

Compiled EE tests exercise SBUS callback installation, malformed registration,
multiple cause delivery with void callback returns, removal, absent handlers,
and invalid causes. GetMemorySize reads actual configured backing size and
rejects an invalid allocation. Native byte stores preserve their access width,
deliver the low byte to a host debug sink, and retain PC on sink failure.
These tests execute generated LLVM code; they do not establish a title screen.

The actual private run now reaches the compiled address-space initialization
routine after cleanup and a loader diagnostic. No mapping provider is installed,
so this is an explicit unsupported stop, not an accepted mapping or title screen.
The new bounded switch layout is checked for index clobbering and malformed table
entries, then compiled and executed by the generated-project test.

### Static native allocation milestone

Compiled synthetic EE fixtures check shared alias read/write visibility, backing
bounds, changed mappings, read-only and invalid device views, native device-range
bounds, reservation limits, unsupported readback, and clearing vacant versus live
ranges. Conversion rejects conflicting backing and permissions. Analysis fixtures
recover table pointers from original synthetic ELF data and reject truncated
records and unrelated service calls. These are native-code acceptance tests;
actual GT4 execution is separately required and does not imply a title screen.

### Packed executable and native handoff milestone

`packed-image` covers original synthetic DEFLATE/section input, malformed lengths,
truncation, trailing data, out-of-RAM sections, missing entry points and conflicting
versus equal overlaps. `packed-llvm-project` unpacks a synthetic container, emits
LLVM, compiles, audits and executes the resulting native program. Compiled EE
checks cover clock sampling, wrap/sign extension, unavailable/regressed sources,
unsupported Random-index reads, and image handoff into a compiled startup entry.
The handoff checks retained arguments after old storage is cleared, reset thread
identity, INTC masking without loss of pending events, and atomic rejection of
invalid or uncompiled entries. Actual secondary-image linking remains distinct
from these synthetic gates and standalone machine compilation.
