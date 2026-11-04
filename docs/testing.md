# Testing AnyPS2

Test major behavior changes with original synthetic inputs and generated native
execution. Run only affected Release tests and relevant integration paths after a
coherent batch of changes. Do not repeat already-passing suites without a new
change, failure or unresolved concern.

## Build and select tests

```powershell
cmake -S . -B build -A x64 -DANYPS2_TEST_PRESENTATION=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release -N
ctest --test-dir build -C Release -R '^formats$' --output-on-failure
```

The last command is an example for a format change; select the affected suites.
Build their targets before running them. A full suite is an explicit audit milestone,
not the default response to each edit.

| Changed area | Relevant test families |
| --- | --- |
| Import and layout | `formats`, `packed-image`, `conversion-inventory` |
| EE discovery and LLVM | `analysis`, `ee-llvm-*`, `ee-pointer-flow`, `ee-object-callbacks`, `ee-flow-*` |
| IOP modules and native communication | `iop-llvm-*`, `native-sif-link` |
| VU compilation and graphics integration | `vu-llvm-execution`, `vu-vulkan-rendering` |
| Vulkan | `native-vulkan-raster`, `vulkan-rendering`, `vulkan-resident-rendering` |
| Input and audio | `pad-input`, `gamepad-host`, `sample-audio` |
| Repository policy | `repository-content`, `repository-policy-tests` |

Consult `ctest -N` for the tests enabled by the current configuration. Diagnostic
hardware/interpreter tests are separate from LLVM native acceptance tests.

## Device and window tests

Automated tests should not open visible windows during ordinary development.
`ANYPS2_TEST_PRESENTATION` defaults to OFF; enable it only for a deliberately
requested presentation test. Opening an actual demo for the user is separate.

`ANYPS2_TEST_AUDIO_DEVICE` and `ANYPS2_TEST_USB_DEVICES` opt into real host devices.
`ANYPS2_TEST_LLVM_GPU` enables the experimental shader test; it is OFF by default.
A synthetic device fixture does not establish physical controller or audio behavior.

## Original PS2 source programs

[The PS2 source runner](ps2-test-programs.md) builds original C++ cases using PS2DEV,
then verifies, converts, compiles and executes them. Select affected cases and
compiler profiles. It covers compiler-generated callbacks, object layouts and stack
patterns that hand-encoded instruction fixtures can miss. PCSX2 comparison is an
optional separate reference; no reference engine is linked into converted output.

## Actual program validation

Commercial runs are opt-in and use locally supplied files. CMake's local startup
fixture accepts `ANYPS2_TEST_PROBE`, `ANYPS2_TEST_IMAGE`, `ANYPS2_TEST_DISC` and a
budget. Keep inputs, generated code, binaries, traces and captures ignored.

After a meaningful integration change, run the actual converted program and inspect
its stopping boundary and subsystem state. Distinguish useful work from polling.
A deadline or expected unsupported stop is not boot success. For graphics changes,
check actual game/demo pixels and interaction; synthetic raster tests alone do not
establish visual correctness. Separate displayed updates, simulation rate and CPU/GPU
cost when reporting performance. See [batch conversion](batch-conversion.md).

`ps2-native --capture-vectors <new-private-directory>` records accepted vector
submissions for the most recent 16 displayed frames into `submissions.tsv`:
microcode uploads, data unpacks (destination, format, count, size, content
fingerprint), direct graphics packets and program launches. It observes after
acceptance and does not alter execution. Use it to measure render structure, such
as which streams are unchanged between frames; keep captures under ignored `work/`.

## Before committing

Run the repository-content guard before staging and again after staging:

```powershell
ctest --test-dir build -C Release -R '^repository-content$' --output-on-failure
```

Review staged paths and content for provenance as well. Passing the guard cannot
establish copyright ownership. Keep validation outcomes in the change description;
do not append per-test diaries to this guide.
