# Testing AnyPS2

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
no commercial game or BIOS. A generated native fixture is compiled by the normal
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
without changing channel state. The local startup regression requires both
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
the original IRX is removed. The C++ pipeline remains covered with `--cpp`.
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
overflow must not register an alarm. Standalone project builds include the host
clock and idle-wait integration; deterministic alarm semantics do not claim exact
wall-clock delivery latency.

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
callback roots. Compiled execution checks budgeted init entry, delayed publication,
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
