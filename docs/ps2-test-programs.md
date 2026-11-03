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

Public homebrew is a separate integration gate. Keep checkouts and generated
artifacts ignored. Record the upstream pin and any local source adaptations;
retain licensing notices. Build the PS2 ELF first, convert it through the normal
LLVM pipeline, then inspect the native window, interaction and audio as applicable.

Current samples and limits are summarized in [project history](project-history.md).
Use the same source and documented adaptations for external comparisons. The
PS2SDK Teapot comparison uses the same ELF as native conversion, including the
SDK helper correction that acknowledges VSYNC without clearing unrelated FINISH.
A successful sample does not prove all callbacks, graphics modes or games work.

Keep test scripts reproducible and reports under ignored output directories. Only
stable usage and known limitations belong here; per-run timings and captured frames
do not need new tracked documents. The experimental LLVM GPU texture path remains
disabled by default after the Dungeon trial; normal rendering uses the CPU/Vulkan path.
