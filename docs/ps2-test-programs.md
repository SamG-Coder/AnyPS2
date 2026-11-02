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

For symbol-bearing homebrew, conversion accepts `--function-symbols` to include
defined ELF function entries and emits `function-roots.tsv`. This does not turn
data or arbitrary executable-segment addresses into functions. It supplements
pointer-flow discovery rather than proving it complete. Public SDK helper code
copied by startup needs explicit `--payloads <mapping>` entries with code-only
extents and proven entry points. No BIOS is involved.

Keep the pinned checkout, local port, payload mapping and generated output under
ignored `third_party/` or `work/`; retain all notices and inspect asset provenance
separately. No upstream code or assets are part of the original fixture corpus.
