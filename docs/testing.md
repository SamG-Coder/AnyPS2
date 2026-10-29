# Testing AnyPS2

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
