# Batch conversion and LLVM builds

`AnyPS2 convert <disc.iso> [new-output-directory] [--jobs N]` inventories
recognizable code, generates all independently convertible targets, and builds
them with LLVM. Without an output argument it creates
`work/<ISO filename without extension>-native/` relative to the current directory.
Existing output directories are rejected; use the generated build to rebuild.

Individual EE and IOP project generators publish their output directory only after
all artifacts have been written and closed. Failed generation removes the owned
temporary directory so the destination can be retried; existing output is preserved.
This does not make the entire disc inventory a single transaction.

The ISO's `SYSTEM.CNF` boot entry selects the executable. Other recognized EE
ELFs, length-prefixed DEFLATE segmented images and IOP modules become independent static libraries. Filenames alone do
not select executable content. The source-to-artifact encoding preserves case,
separators and long names even on case-insensitive filesystems.

Allocated ELF `.vutext` containers are retained as VU input and excluded from
EE function discovery, even when the enclosing LOAD has its executable flag set.
The inventory reports them as unconverted `vu-container` units until their
upload framing and VU entry points are recovered. Reading or storing a pointer
to those bytes does not make them an EE function; an actual EE branch into a
declared VU container remains an unresolved conversion error.

| Output | Purpose |
| --- | --- |
| `bin/ps2-native.exe` | Native boot executable, when its reached code converts |
| `bin/program.data` | Initialized data copied beside newly generated executables |
| `libraries/*.lib` | Converted code and linked native support libraries |
| `ee/`, `iop/` | Retained LLVM IR, module manifests, source mappings and local data |
| `build-plan.json` | Target roles, planned output paths, generation/compilation coverage and grouped gaps |
| `report.txt` | Human-readable inventory and missing-work report |
| `library-calls.json`, `library-calls.md` | All discovered call identities, source ownership, binding candidates and researched native mappings |
| `build-result.txt` | Separate receipt for a successful batch build; execution is not implied |
| `build/` | Reusable CMake build, objects, link maps and optional compiler reports |

Use `AnyPS2 inventory` instead of `convert` to compile independent objects
without linking, or add `--no-compile` to inventory for generation only. The
inventory counts a unit as compiled only after **every** LLVM module succeeds.
Independent units continue after a conversion or object-compilation failure.
`convert` uses CMake for compilation rather than compiling the same objects twice.
The default concurrency is at most four compiler processes; `--jobs 1..64`
overrides it. Start with four on large inputs and measure memory use before
raising the limit.

Use `AnyPS2 inventory <disc.iso> <new-output-directory> --audit-only` to collect
the call and coverage reports without generating or compiling game code. The
call list includes all parsed IOP import stubs, including imports in failed or
unreached modules, and recognized EE syscall candidates. Each IOP entry preserves
the exact library version, ordinal, source and analysis address. Matching exports
are candidates until module compilation, registration and execution are verified.
Unknown selectors, private interfaces and incompatible versions stay unresolved.
Dynamic calls, RPC method selectors, direct device accesses and unidentified code
are outside this list's completeness claim.

The mapping report separates existing native services from open-source candidates,
records license/reference links, and lists the ABI and ordering requirements for
each adapter. It never installs a library or changes runtime binding based on a
name match. The public name catalogue is conversion tooling only; it is not a
runtime instruction or address lookup table. See
[source references](source-references.md#offline-library-call-mapping) for provenance.

To inspect a blocked IOP function and its surrounding code without running the
game, use `AnyPS2 inspect-iop <module.irx> <base-hex> <new-report.md>
--capture <stop-directory> --radius <hex-byte-radius>`. An explicit
`--anchor <hex-address>` can replace the capture. The tool selects the observed
PC when it belongs to the module, or its return call site otherwise. It groups
nearby imports using the existing public API catalogue and reports local
constant arguments, stack-built request bytes, callback/address candidates,
function-entry candidates and undecoded words. Use these groups to choose a
coherent native-service batch before rebuilding. The report contains private
input-derived data and belongs under ignored `work/`.

For captures that can stop in different modules, use `AnyPS2 inspect-iop
--bundle <new-report.md> --capture <stop-directory> --radius <hex-byte-radius>
--modules <base-hex> <module.irx> ...`. Bundle bases accept the same `sealed@`
and `romdrv@` prefixes as the converter. The inspector locates the caller's
module from the supplied relocation ranges; overlapping owners are rejected.

This is a bounded static neighborhood, not a proof that every scanned word is
executable or reachable. Unknown values stay unknown; embedded data can produce
undecoded words. Static candidates neither bypass runtime checks nor authorize
successful service responses.

```powershell
# Explicit destination, or substitute inventory to skip linking.
.\build\Release\AnyPS2.exe convert 'path/to/owned-game.iso' work/game-native --jobs 4

# Rebuild retained IR without importing or generating the disc again.
cmake --build work/game-native/build --config Release --parallel 4

# Select another installed Clang. No game-code source rewrite is required.
cmake -S work/game-native -B work/game-native/build -DANYPS2_CLANG='C:/LLVM/bin/clang.exe'
cmake --build work/game-native/build --config Release --parallel 4

# Optional per-module timing JSON and optimization remarks (may be large).
cmake -S work/game-native -B work/game-native/build -DANYPS2_LLVM_REPORTS=ON
cmake --build work/game-native/build --config Release --parallel 4
```

`--compiler <path>` selects Clang during initial conversion; `--source-root`
overrides the build-time AnyPS2 source location. Generated EE/IOP projects share
one LLVM build implementation. The compiler file is a dependency, so changing
the installed compiler rebuilds affected objects. Unchanged modules are reused.
Reports are opt-in and use Clang's optimization-record and time-trace flags;
see the [Clang user manual](https://clang.llvm.org/docs/UsersManual.html).

`ANYPS2_LLVM_OPT_LEVEL` accepts `0`, `1`, `2` or `3`, with `2` as the default.
Changing it rebuilds generated EE/IOP objects. A higher level is an experiment,
not a promise of faster gameplay. Fast-math is not enabled. LLVM's standard
pipeline performs instruction combining and other transforms described in the
[LLVM pass reference](https://llvm.org/docs/Passes.html). AnyPS2 first preserves
guest semantics in typed lowering so these optimizations operate on native IR.

IOP output keeps bounded 256-instruction functions, grouping up to sixteen
address pages in each LLVM compilation unit. Dispatch and shared code guards
are separate. EE output also retains a manifest of bounded compilation units.
This permits parallel compilation and incremental rebuilding without a runtime
compiler or instruction decoder.

EE lowering recognizes `LUI` followed by same-register `ORI`, `ADDIU` or `DADDIU`
as constant materialization. Its fast path writes the combined value and retires
both operations. Original entry points remain usable, including one-operation
budgets; delay slots and changes in image/payload ownership prevent unsafe
grouping. Word sign extension, wraparound and untouched upper lanes are retained.
Future patterns belong in the converter with compiled behavior regressions.

## Coverage and measurement limits

For repeated startup validation of an existing converted project, run
`cmake -C <private-settings.cmake> -P cmake/validate_native.cmake`. The settings
file supplies `SOURCE_ROOT`, `NATIVE_BUILD`, `NATIVE_DATA`, `DISC`, `IOP_DATA`,
`TEST_TARGETS` and the CTest regex `RELATED_TESTS`. It rebuilds the selected
test targets, runs only those Release tests, rebuilds the native executable,
and runs it headlessly with a bounded duration. Set `RUN_SECONDS` and `JOBS`
to override the defaults of 120 seconds and four workers.

For an IOP conversion change, also set `IOP_INPUTS_JSON` to a private JSON array
of arguments for `ps2-iop-recompile --bundle`, and `NATIVE_PROJECT` to the
existing generated EE project. Include `ps2-iop-recompile` in `TEST_TARGETS`.
The command regenerates the IOP bundle and reconfigures the existing executable
before building. Service-only changes reuse the generated bundle.

Each run saves build/test logs, the native log, memory/framebuffer captures,
and previous/current summaries under ignored `work/native-validation/`.
A matching boundary is reported as no verified forward progress. A changed
boundary still requires review; operation counts, polling, or a successful
pipeline never establish a title screen or gameplay. Use a normal visible
launch after a batch when inspecting the actual game output.
Optional `INSPECT_MODULE`, `INSPECT_BASE` and `INSPECT_RADIUS` settings add the
offline neighborhood report to each captured run; include `ps2-iop-inspect` in
`TEST_TARGETS` so the inspector is rebuilt with the batch.
Use `INSPECT_INPUTS_JSON` with the converter's base/module argument array to
select the stopped caller's module automatically. Regeneration through
`IOP_INPUTS_JSON` already supplies that array. A failed inspection keeps the
captured execution boundary and reports its own error separately.

Libraries are independent conversion artifacts, not a claim that every module
has been selected and bound into a working game. Cross-module initialization,
load order, native service providers and unknown indirect targets still need
validated linkage. VU code requires a proven upload packet or declared ELF overlay metadata; arbitrary disc bytes
are not guessed to be instructions. Packed/encrypted/transformed content that
the disc walk does not recognize remains explicitly undiscovered. Unsupported
services are listed, never replaced by generated success stubs.

For a declared contiguous VU program image, `ps2-vu-recompile input.bin output.ll
0 0 --all-entries` compiles every pair as a possible native entry (the numeric
arguments select unit and destination pair). This covers indirect launches and
entries after earlier E stops without execution observations. Unsupported
instructions or missing branch/E delay pairs reject conversion. A plain tail
executes its compiled instructions and reports an unmapped boundary if it leaves
the image; it does not become an invented successful return. Compile and link
the emitted LLVM module through the existing VU binding configuration.
A trailing `--gpu` emits the same blocks as one Vulkan compute kernel instead
([whole-program VU kernels](render-modernization.md#whole-program-vu-kernels));
continuation discovery is not available for that target.

`ps2-vu-extract program.elf new-output-directory --elf` recovers every region
declared by a DVP overlay table from its file-backed load address. It writes
numbered payloads and `overlays.tsv` with source locations, destination byte
addresses and sizes. Shadow-section contents are not used as instructions.
`images.tsv` also groups adjacent destination ranges when only NOPs and the
matching upload command separate their payloads. The table does not identify
the VU unit; unit selection still requires validation.

`ps2-vu-extract program.elf new-output-directory --scan-elf` handles programs with
no overlay table. It finds uploads stored ready to send: a qword-aligned DMA tag
whose size exactly frames VIF uploads that assemble into an image with an end marker.
Its `images.tsv` is a catalog. Validate the unit by converting each image for it.

Uploads may target any free microcode address, because compiled branches are relative.
A launch resolves to the catalog program occupying that address. An absolute address
the EE computes from the load location and passes through data is not translated.

For multiple images on one unit, pass `ps2-vu-recompile --catalog images.tsv
output.ll 1`. This manifest has two tab-separated columns, `file` and
`destination_pairs`; file paths are relative to the manifest. The generated
module exports native functions for every declared pair in every image. Link
it using the existing VU module option. Uploads validate identity and invalidate
overwritten coverage; unknown images and incomplete programs cannot execute.

Generation time, LLVM compile time, linking, native execution and rendering are
different measurements. The window's presentation FPS counts successful
presentations over host elapsed time; it is not unique game-frame throughput or
a measure of how close the game is to its intended speed. Retired operations can
include polling. Benchmark matched workloads and compare stop state as well as
wall time before claiming a runtime speedup. Commercial inputs and all generated
artifacts remain private under ignored `work/`.
