# Original PS2 test programs

`tests/ps2_programs` builds original C++20 programs into real R5900 ELF files with
the open-source PS2DEV GCC toolchain. The same ELF is then passed through AnyPS2's
normal discovery, LLVM emission, compilation, link and native execution stages.
An optional external PCSX2 run checks the same program's results independently.
GCC builds the test inputs only; AnyPS2's output backend remains LLVM.

This complements handwritten instruction fixtures. A real compiler produces
prologues, stack frames, register allocation, delay slots, return-value handling
and vtables that instruction fixtures can miss. Inputs are built separately at
`-O0` and `-O2` by default, without LTO or manually supplied executable roots.
Compiler helpers in separate translation units preserve real ABI boundaries
without disabling ordinary compiler optimization.

## Current corpus

The ladder isolates simple behaviors before combining them. A higher level is
more demanding, not a claim that all lower-level cases pass.

| Level | Programs | Behaviors |
| --- | --- | --- |
| 1 | `scalar`, `direct_flow` | Integer widths, partial data writes, ten arguments, direct calls, both branch paths, loops and empty loops |
| 2 | `callback_arguments`, `callback_stack`, `callback_mutation` | Callback arguments across translation units, stack aliases, out-parameter updates, mutable global pointers and preserved copies |
| 3 | `callback_returns`, `callback_stack_arguments`, `callback_tables` | Both factory return paths, a callback beyond eight register arguments, indexed records and table mutation |
| 4 | `callback_copy`, `callback_context`, `callbacks` | Aggregate/partial pointer byte copies, adjacent field writes, nested contexts, keyed registration and lookup |
| 5 | `objects`, `multiple_inheritance`, `callback_queue` | Virtual factories, secondary-base adjustor thunks and deferred FIFO callbacks with submissions while draining |

The full corpus returns failure while any build or behavior check fails.
Do not turn a missing target into an expected successful outcome. Exact source,
ELF, converter and native-executable identities, coverage reports, execution
boundaries and observed values are recorded under the selected output directory.
These tests establish neither GT4 boot nor complete callback discovery.

The first full ladder audit (PS2DEV GCC 15.2.0, PCSX2 2.8.2, Windows x64) built
all 28 `O0`/`O2` inputs; both reference CPU modes matched every expected result.
Native execution passed `scalar`, `direct_flow`, `callback_arguments` and
`callback_stack_arguments` in both profiles. The other ten families stopped at
missing compiled targets. The gate does not count their successfully generated
and linked partial programs as behavioral passes.

A second audit checked `scalar`, `callback_stack` and `multiple_inheritance`
with `Os`, `O2-fp` and `O2-relocated`. All nine inputs passed both reference modes.
The two additional `scalar` O2 profiles passed natively; `scalar-Os` exposed
discovery following an impossible fallthrough after an always-taken `BEQ`.
That bug was fixed and the same ELF then passed LLVM generation, native linking,
dependency audit and execution in a targeted rerun. The callback and inheritance
failures remain unresolved. This records the exercised configurations, not an
all-platform or complete-code-coverage claim.

## Build and run

Install a PS2DEV release outside tracked source. The verified Windows setup uses
PS2DEV `v2.0.0`, GCC `15.2.0`, and matching **32-bit MinGW host libraries**. Put the
MinGW32 DLL directory on `PATH`, or pass it with `--host-runtime`. A 64-bit DLL
with the same name cannot satisfy the 32-bit compiler. No system-wide toolchain
or execution-policy change is required. See [pinned provenance](source-references.md#original-ps2-test-toolchain).

Build the PS2 ELFs for the default two compiler profiles:

```powershell
cmake -S tests/ps2_programs -B work/ps2-programs -A x64 -DPS2DEV=<toolchain-directory>
cmake --build work/ps2-programs --config Release --parallel 2
```

Run the complete build/conversion/execution audit:

```powershell
python tests/ps2_programs/run.py --ps2dev <toolchain-directory> --converter build/Release/ps2-recompile.exe --output-root work/ps2-test-results
```

The runner creates a fresh evidence subdirectory, builds only selected targets,
and continues through the other cases after an individual build, conversion or
runtime failure. Use `--list` to inspect the ladder without a toolchain,
`--max-level 2` to stop at simpler patterns, or repeated `--case <name>` options
to focus on a regression. Case and level filters intersect; an empty selection
is an error. `results.json` and `results.tsv` distinguish PS2 compilation,
verification, LLVM generation, native compilation, execution and external
reference results. Verification can remain incomplete even when a particular
runtime path passes.

During iteration, select only the new or affected case/profile pairs. Keep the
prior reference evidence when the ELF is unchanged. A full matrix is an explicit
audit milestone, not something to repeat after each small edit.

Repeat `--profile` to select configurations from `cases.json`:

| Profile | PS2 input build |
| --- | --- |
| `O0` | Unoptimized |
| `O2` | Optimized, compiler-default frame-pointer policy |
| `Os` | Optimized for size |
| `O2-fp` | Optimized with a retained frame pointer |
| `O2-relocated` | Optimized at a different link address |

For example, add `--case callback_stack --profile O0 --profile O2-fp
--profile O2-relocated` to compare the same behavior across stack-frame and
image-layout choices. Direct CMake builds accept
`"-DPS2_TEST_PROFILES=O0;O2;Os;O2-fp;O2-relocated"`. Generated output still uses
the existing Release Windows x64 native runtime and LLVM optimization level 2.
Changing the input profile does not change that native backend.

Enable the level-one regressions, size-optimized discovery regression and
protocol checks in Release CTest:

```powershell
cmake -S . -B build -A x64 -DANYPS2_TEST_PS2DEV=<toolchain-directory>
cmake --build build --config Release --target ps2-recompile --parallel 4
ctest --test-dir build -C Release -R '^ps2-source-' --output-on-failure
```

`ANYPS2_PS2_HOST_RUNTIME` optionally supplies the MinGW32 DLL directory to CTest.
The complete callback/object audit remains a separate explicit invocation.
Passing the level-one gates does not conceal failures in harder patterns.

## External reference

Add `--reference-exe <pcsx2-qt.exe> --bios <user-owned-bios-file>` to the audit
command. The adapter was exercised with PCSX2 `v2.8.2` on Windows. It creates an
isolated configuration and loopback PINE endpoint for each owned process, reads
the test result block, records reference version/hash and closes that process.
It does not modify an existing PCSX2 session, controller configuration or saves.
Reference failure is reported separately and does not suppress native testing.
Add `--reference-mode recompiler --reference-mode interpreter` to run both
external CPU configurations for each ELF. Each has its own configuration,
process, observations and identity report. This is a comparison between reference
implementations; AnyPS2 always uses LLVM AOT execution.

AnyPS2's converted executable uses no BIOS and does not link or invoke PCSX2.
The BIOS is only an optional input to the separate reference application and is
never copied into the project. A reference match is not physical-hardware proof.
The ELF can also be loaded on a PS2 using a homebrew loader; hardware execution
has not been verified here. These checks do not establish behavior on every
operating system, CPU architecture, compiler version or physical PS2 revision.

## Adding programs

Add a C++ source containing `test_body()` and an entry in `cases.json` with a
unique ID, level, feature description, source list and independently derived
expected values. Reuse `record()` and the
shared freestanding entry/link script. The result block contains a magic number,
schema version, case ID, completion marker, observation count and up to 16 words.
After publication the test waits in an explicit completion loop. The native
runner requires both that loop and the complete expected result; consuming an
operation budget is not a pass by itself.

The present suite links no SDK or console kernel code. Future IOP module, VU,
graphics and service tests need explicit additional build targets and their own
behavior contracts. Do not claim those families are covered by these EE tests.
Keep generated ELFs, objects, LLVM, executables, reference state and logs under
ignored `work/` or `build/`. Only the original source and expectations belong in Git.

## Larger homebrew tests

The [glampert cube demo](https://github.com/glampert/ps2-homebrew/tree/602441a6877a3136709d6320664340d52e3027a1/source/demos/cubes)
is a useful next graphics integration target after these smaller programs: its
source exposes the intended cube geometry, checker texture, camera and input
behavior. The console demo is a smaller service integration step; audio and the
dungeon game add more dependencies. Such programs complement the isolated
patterns rather than replacing their precise expected observations.

The local compatibility port builds all ten source files and links a PS2 ELF.
Changes cover declarations, POSIX file access, obsolete GNU assembly syntax,
assembly memory clobbers, cast assignments and a controller wrapper that used
uninitialized state after a failed module load. The local SDK sync helper also
acknowledges only its requested event, retaining independent draw completion.
The PS2 ELF converts through LLVM AOT and renders the rotating checker-textured
cube grid and text in a 640x448 Vulkan window. A 20-second visible run completed
at its requested diagnostic deadline without an unsupported-operation stop;
captures at different rotations verify changing scene pixels. The native SDL
controller provider subsequently completed a 120-second run, and the user
confirmed physical DualSense camera input. The source port selects
`ANYPS2_NATIVE_PAD` to bind native legacy-pad endpoints instead of loading ROM
modules. The original camera uses inverted vertical look. This does not establish
GT4 progress or hardware-exact rendering. Edge antialiasing uses native 4x4 coverage.

Controller regressions are `pad-input` (subscription lifecycle, packet layout,
aliases and memory protection) and `gamepad-host` (SDL virtual-device mapping and
disconnection). The virtual device is test-only. Scalar camera arithmetic is
covered by generated-code MADD/MSUB/MADDA/MSUBA underflow regressions in
`ee-llvm-execution`. Native pad commands outside the implemented legacy subset
remain explicit failures. Native vibration follows actual SDL capability and
host success/failure; pressure-sensitive buttons are not exposed. The converted Console/Input
demo displays real controller button reports; the user verified buttons and
vibration on a DualSense: L1/R1 start the small/large motor,
and L2/R2 stop them. These are latched commands: releasing L1/R1 does not
stop vibration. Fully squeeze both triggers to stop both channels; closing the
native window also cancels vibration. Simultaneous start/stop buttons are processed
in the original source order, with each stop after its matching start.
Create maps to Select and Options maps to Start. Face-button
pressure values remain zero because the provider does not advertise pressure
sensors. The original source calls its vibration channels small/big; their
physical feel depends on the PC controller.

The **Audio/Sound** demo also builds to a PS2 ELF, converts through LLVM and
runs in a native Vulkan window. The local `ANYPS2_NATIVE_AUDIO` port replaces
ROM/module loading with binding to the native audsrv provider. Its public EE
client still performs allocation, upload, RPC completion and playback calls.
The wrapper accepts any nonnegative channel returned by the modern SDK. The
provider owns transfer buffers and decoded mono APCM version 1 samples; XAudio2
owns the queued PCM and reports actual consumption. Unsupported formats and
history-dependent loop starts stop explicitly. Native resampling is used rather
than console interpolation, so this is not a bit-exact audio claim.

`sample-audio` covers original synthetic samples, malformed/unsupported data,
atomic uploads, memory protection, sample lifetime and playback state.
The opt-in `native-audio-device` test measures real silent sample consumption,
looping, stopping/replay and coexistence with an existing COM STA apartment.
`native-sif-link` covers the affected communication path. In the local demo run,
the user confirmed audible playback and reported it sounds about the same as
the PCSX2 reference. The reference build loads the public audsrv module, using
the public SDK module-buffer loader patch required by that older environment;
the native output does not use this patch, BIOS or emulator.

Dungeon's PNG loader exposed bounded-switch
patterns with a table reloaded before index scaling and a range predicate hoisted
across branches and a loop. The converter now tracks those guards conservatively;
`analysis` checks conflicting paths and overwrite rejection, while
`ps2-source-switch-spill` compiles and executes all sixteen original test outputs.

The menu-to-level transition also exercises scalar arithmetic outside IEEE
single-precision's finite range. The LLVM scalar path now supports exponent-255
values, signed overflow saturation, scalar comparison, division and square root,
and the accumulator overflow latch used by MADD/MSUB. `ee-llvm-execution` covers
these results and flags, including delay slots and host rounding-mode independence.
The shared VU arithmetic also accepts extended values and reports lane overflow
flags. Opposing-sign accumulation after an overflowing product and intermediate
product underflow remain unsupported. `vu-llvm-execution` checks supported results
and atomic rejection of those unvalidated cases.
The demo's camera initialization and fade logic are not patched. A 180-second
menu run completed at its diagnostic deadline with sample playback and no
unsupported stop. It recorded no button changes or 3D triangles, so the level
transition and Dungeon gameplay were not established by that run. A subsequent
controller-driven run passed the camera/frustum arithmetic and submitted triangles,
but stopped on HIGHLIGHT texture combining before the user saw a 3D scene.
HIGHLIGHT/HIGHLIGHT2 are now covered by GPU readback tests for all four texture
functions, alpha selection and saturation. The user subsequently confirmed that
Dungeon renders its 3D scene. Visual parity and sustained gameplay remain under
verification; the user reported responsive menus but lag during 3D execution.

The production renderer now uses Vulkan graphics pipelines (`vkCmdDraw`) and
linear native texture and colour/depth images. The depth image is an integer
attachment: fragment operations preserve 32-bit comparisons rather than losing
low bits through floating-point depth conversion. Native image snapshots retain
framebuffer-texture feedback. Palette/texture uploads and explicit diagnostic
exports translate storage layout only at the resource boundary. Unusual aliases
between different image views may still require a synchronization and conversion;
normal display output samples the native images directly into the swapchain.

Generated builds exclude the old compute renderer and its shaders. Repository
reference tests opt into it explicitly. There is no automatic renderer fallback.
The native path requires Vulkan conservative rasterization for the currently
supported antialiased primitive coverage. Triangle interiors retain full opacity
and depth writes; the 4x4 sampled exterior fringe remains an experimental
approximation, not a claim of exact GS antialiasing. Synthetic
pixel equality with the reference tests is not proof of complete PS2 fidelity.

`native-vulkan-raster` compares hardware rasterization against the reference for
texturing, feedback, masks, alpha tests, integer depth and antialiased triangles.
`vu-vulkan-rendering` exercises compiled VU drawing through the native path.
The window reports rendered display updates, not monitor refresh or guaranteed
game-loop FPS. Repeated submissions with unchanged display resources are omitted;
the counter does not test pixel equality. The upstream Dungeon README reports
30 FPS on console and emulator; use the demo's own FPS overlay for loop timing.
Diagnostic controller polling gaps are not end-to-end input latency.

Native resource uploads append to bounded staging arenas. Three fenced submission
slots retain upload bytes and command buffers until the GPU releases them. FINISH
uses native queue completion tokens: consuming a packet does not assert that its
rendering has completed. Status reads poll the real fences, and explicit waits,
resource destruction and diagnostic exports drain the required work. Disjoint
screen regions share a dependency epoch; overlapping regions retain attachment
read/write ordering. The antialiasing shader takes an exact shortcut within the
exterior sampling rule for fully covered samples and uniform vertex colours.
Small interpolation quotients use floating-point estimates verified and corrected
with integer arithmetic; uncertain cases retain full integer division. RGBA upload
words are consumed directly while preserving row boundaries and packet padding.

Consecutive compatible primitives in disjoint screen regions use instanced
Vulkan draws backed by bounded, fenced draw-record buffers. Overlapping regions,
render-target changes, texture uploads and FINISH preserve their ordering
boundaries. Per-record clipping retains each primitive's scissor. Native packet
delivery also groups up to 256 consecutive draws per CPU call; palette loads,
uploads and completion operations flush that group. Texture lookup keys use
fixed storage and unchanged pipeline/resource bindings are reused.
On devices supporting ordered fragment interlock, overlapping primitives also
share draws. Colour/depth tests, masked writes and blending execute in an ordered
per-pixel critical section over GPU images; interpolation and texture sampling
remain outside it. Explicit primitive barriers remain in the native attachment
path for devices without that feature. Both use the same pixel calculations.
The profiler reports `ordered_fragment_access` so capability selection is visible.
Graphics packet payloads are copied in contiguous spans, and image uploads
consume whole words with row tracking shared across each span. Split packets,
24-bit carry bytes, indexed nibbles, write masks and address wrapping retain
their original semantics.

Native presentation permits two frames in flight instead of draining the queue
after every present. Acquire semaphores are reused only after their submission
fence completes; render-complete semaphores belong to individual swapchain
images. Rendering and FINISH remain ordered on the same queue. This changes
host scheduling, not game timing or the number of generated display updates.
Release builds inline the measured vector lane/add/multiply helper chain to
reduce native call and temporary-copy overhead. Their integer-significand
arithmetic, flags and exceptional-result handling are unchanged; no fast-math
or fused floating-point substitution is enabled.
Where AVX-512 and the required OS register state are available, bounded normal
vector add/subtract/multiply/accumulate operations use explicit chop rounding
without changing the host floating-point environment. Exceptional ranges keep
the integer arithmetic path. Regression comparisons cover masks, aliases,
broadcasts, signed zero and all host rounding modes; multiply and accumulate
remain separately rounded.

The native probe accepts `--profile-graphics` for five-second timing samples and
`--validate-graphics` to enable Vulkan validation. Normal runs omit validation;
GPU regressions keep it enabled. CPU draw, upload, write and wait timers overlap,
so they must not be summed. GPU timestamps measure completed queue work, while
CPU submission/wait timings include driver and scheduling cost. Unsupported GPU
timestamps are explicitly identified. Samples distinguish triangles, barriers,
uploads and submission counts, plus median/95th-percentile intervals between
rendered display updates. These are not simulation-frame or input-latency
measurements; repeated presentation does not become game FPS.

Profiling additionally reports actual draw calls and batched primitives,
compiled-entry wall time (including native calls), syscall time, scheduler sleep,
status-read counts and display-buffer changes. VSYNC clear-to-observe time measures
the interval from acknowledging the event to first observing its next occurrence;
it is not a CPU-utilization measurement. These intervals can overlap the other
timers. In particular, a high retired-operation count during repeated status reads
does not establish useful game execution or a higher game frame rate.

The Vulkan regressions additionally cover disjoint/overlapping batches, staging
arena exhaustion, deferred completion and reuse of in-flight upload slots. The
draw-record tests also cover slot rollover, per-instance triangle clipping,
mixed coverage modes, and CPU batch boundaries crossed by ordered FINISH commands. The
opt-in `native-vulkan-presentation` test repeatedly reuses swapchain resources
with synchronization validation, alternates source images, checks unchanged-frame
rejection and verifies final pixels and FINISH completion. Bulk-transfer tests
cover every quadword split point, unaligned input, padded image tails, wrapped
addresses and malformed packet lengths. The
compiled EE test verifies that pending graphics work is not reported as FINISH
and backend failures remain explicit diagnostic stops. Synthetic comparisons do
not establish sustained Dungeon performance or complete PS2 graphics fidelity.

After the native submission and antialiased-interior fixes, a manual Dungeon
check confirmed that the player's armour looked solid and movement was smooth.
Five-second 3D samples in the preceding performance run reached approximately
30 rendered updates/s with 95th-percentile update intervals near 34-35 ms,
compared with roughly 50 ms in the heavier views before the final optimizations.
These are local display-update measurements, not an asserted game-frame cap,
a hardware-wide performance guarantee, or proof of complete compatibility.

For symbol-bearing homebrew, conversion accepts `--function-symbols` to include
defined ELF function entries and emits `function-roots.tsv`. This does not turn
data or arbitrary executable-segment addresses into functions. It supplements
pointer-flow discovery rather than proving it complete. Public SDK helper code
copied by startup needs explicit `--payloads <mapping>` entries with code-only
extents and proven entry points. No BIOS is involved.

Keep the pinned checkout, local port, payload mapping and generated output under
ignored `third_party/` or `work/`; retain all notices and inspect asset provenance
separately. No upstream code or assets are part of the original fixture corpus.

The experimental `ANYPS2_LLVM_TEXTURES` build option compiles native texture
import to Vulkan compute through LLVM. Configure `ANYPS2_GPU_CLANG`,
`ANYPS2_SPIRV_LLC`, `ANYPS2_LLVM_LINK` and `ANYPS2_LLVM_OPT` to compatible LLVM
22.1 tools with the SPIR-V backend, and provide `spirv-val` from the Vulkan SDK.
The build retains LLVM, linked/optimized bitcode and validated SPIR-V. Runtime
code loads the embedded shader; it does not invoke a compiler. This option
requires Vulkan 1.3 with `shaderInt8` (LLVM currently emits byte constants for
resource names) and keeps decoded texture output on the GPU.
For an integration diagnostic, `ps2-native --verify-textures` compares each
executed texture kernel with the shared CPU address and palette rules. A
mismatch stops with its texel coordinates. This explicit verification mode
reads results back and is not suitable for performance measurements; ordinary
execution does not perform these comparisons.

`ANYPS2_TEST_LLVM_GPU` adds an independent arithmetic-kernel execution test.
The `llvm-gpu-kernel` and `native-vulkan-llvm-textures` Release tests check the
compute results and renderer integration respectively. Arithmetic graph
lowering and native texture import are separate from automatic extraction of
EE/VU geometry loops; enabling them does not move those game loops to the GPU.

Both experimental LLVM GPU options default to OFF. The local Dungeon trial
passed texture comparisons but was reported slower with diagnostic readbacks
enabled. The demo was returned to CPU texture conversion with native Vulkan
drawing. No performance benefit or GPU conversion of its geometry loop is claimed.
