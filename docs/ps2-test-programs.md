# Original PS2 test programs

`tests/ps2_programs` builds original C++20 programs into real R5900 ELF files with
the open-source PS2DEV GCC toolchain. The same ELF is then passed through AnyPS2's
normal discovery, LLVM emission, compilation, link and native execution stages.
An optional external PCSX2 run checks the same program's results independently.
GCC builds the test inputs only; AnyPS2's output backend remains LLVM.

This complements handwritten instruction fixtures. A real compiler produces
prologues, stack frames, register allocation, delay slots, return-value handling
and vtables that instruction fixtures can miss. Inputs are built separately at
`-O0` and `-O2`, without LTO or manually supplied executable roots.

## Current corpus

| Family | Behaviors | Native result when introduced |
| --- | --- | --- |
| `scalar` | Unsigned overflow, 64-bit shifts, division, partial byte writes, ten arguments, stack locals and branches | Both variants match the specified values and PCSX2 |
| `callbacks` | Returned function pointers, helper calls, keyed registration/lookup, mutable callbacks and absent keys | Both variants stop at missing compiled targets; PCSX2 matches the specified values |
| `objects` | Constructors, virtual dispatch, base-pointer factory returns and copied pointer aliases | Both variants stop at missing compiled targets; PCSX2 matches the specified values |

The full corpus deliberately returns failure while these discovery gaps remain.
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

Build just the six PS2 ELFs:

```powershell
cmake -S tests/ps2_programs -B work/ps2-programs -A x64 -DPS2DEV=<toolchain-directory>
cmake --build work/ps2-programs --config Release --parallel 2
```

Run the complete build/conversion/execution audit:

```powershell
python tests/ps2_programs/run.py --ps2dev <toolchain-directory> --converter build/Release/ps2-recompile.exe --output-root work/ps2-test-results
```

The runner creates a fresh evidence subdirectory, builds all inputs, and continues
through every selected case even after a conversion or runtime failure. Use
`--case scalar`, `--case callbacks` or `--case objects` to select families; the
option can be repeated. `results.json` distinguishes verification, generation,
compilation, native execution and reference execution. Verification can remain
incomplete even if a particular runtime test passes.

Enable the established scalar regression and protocol checks in Release CTest:

```powershell
cmake -S . -B build -A x64 -DANYPS2_TEST_PS2DEV=<toolchain-directory>
cmake --build build --config Release --target ps2-recompile --parallel 4
ctest --test-dir build -C Release -R '^ps2-source-' --output-on-failure
```

`ANYPS2_PS2_HOST_RUNTIME` optionally supplies the MinGW32 DLL directory to CTest.
The callback/object gap audit remains a separate explicit invocation so the
scalar gate cannot conceal its four known failing variants.

## External reference

Add `--reference-exe <pcsx2-qt.exe> --bios <user-owned-bios-file>` to the audit
command. The adapter was exercised with PCSX2 `v2.8.2` on Windows. It creates an
isolated configuration and loopback PINE endpoint for each owned process, reads
the test result block, records reference version/hash and closes that process.
It does not modify an existing PCSX2 session, controller configuration or saves.
Reference failure is reported separately and does not suppress native testing.

AnyPS2's converted executable uses no BIOS and does not link or invoke PCSX2.
The BIOS is only an optional input to the separate reference application and is
never copied into the project. A reference match is not physical-hardware proof.
The ELF can also be loaded on a PS2 using a homebrew loader; hardware execution
has not been verified here.

## Adding programs

Add a C++ source containing `test_body()` and an entry in `cases.json` with a
unique ID and independently derived expected values. Reuse `record()` and the
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
