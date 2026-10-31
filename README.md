# AnyPS2

AnyPS2 is a generic, BIOS-free C++20 toolchain with the goal of converting a
user-supplied PS2 ISO into a **modern native PC executable using LLVM ahead-of-time compilation**.
Gran Turismo 4 is the first integration target.

**The required output is an ahead-of-time compiled native program.** EE, IOP and VU program code must be translated during
conversion and compiled into native host code. The final program must not fetch,
decode or interpret PS2 instructions at runtime, use a JIT, or fall back to PCSX2
or another emulator. Unsupported code must produce a conversion diagnostic.

Conversion should also **modernize how the program works on a PC**. The converter
should identify the purpose of console-specific operations and lower them to
native service calls that use suitable PC systems instead of reproducing the PS2 hardware structure
by default. Vulkan rendering is one example; the same approach applies to audio,
input, files, memory management, threading and synchronization. Replacements must
preserve the game's observable behavior, including ordering and numeric details
where they matter. These are conversion goals, not claims of completed support.

LLVM is the only game-code output backend. Typed AnyPS2 IR lowers to LLVM IR,
which is compiled ahead of time and linked with reusable C++20 native services.
There is no generated C++ game-code backend or C++ fallback. Retain local IR and
build artifacts for reproducibility; keep converted commercial content ignored.
The standalone IOP converter is implemented for a bounded subset. EE/VU LLVM
conversion and integration into game startup remain unfinished.
**GT4 renders its opening warning through Vulkan, but has not reached the title screen or playable gameplay.**
No performance improvement is claimed.

## Pipeline

```text
User-owned PS2 ISO
  -> import and validate boot ELF, modules and program data
  -> discover EE, IOP and VU code, including loadable modules
  -> lift into typed AnyPS2 IR and recover functions/control flow
  -> modernize supported operations into native service calls
  -> lower to LLVM, compile ahead of time and link BIOS-free C++ services
  -> native PC executable and local game data
```

This is the target pipeline; complete IOP/VU conversion and LLVM integration into
the main game startup path remain unfinished.
Native services must preserve the game's observable
graphics, audio, input, file-I/O and synchronization behavior. Vulkan is the
graphics backend. These services must not hide an instruction interpreter or a
whole-machine emulation loop. PCSX2 is an open-source behavior reference; it is
not linked or bundled.

## Build and test

Requires CMake 3.24 or later, a C++20 compiler and a C compiler for the pinned
zlib conversion dependency. Initial configuration downloads zlib 1.3.1 with a
verified archive hash; generated game executables do not link this importer dependency. The tested platform is Windows
x64 with Visual Studio's C++ workload. Install the Vulkan SDK (including `glslc`
and validation layers) and a Vulkan-capable graphics driver. GS solid-sprite
drawing now uses Vulkan compute; there is no silent CPU rendering fallback.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release -R "^iop-llvm-" --output-on-failure
```

Tests use synthetic fixtures, including generated LLVM IR that is compiled and
executed. See [testing](docs/testing.md) for opt-in local game validation.

Run `.\build\Release\anyps2-display.exe` for the Vulkan presentation host. It
displays an original synthetic GS sprite scene, not game output. `--smoke`
presents three frames and exits. The initial host uses a fixed-size native
Windows window; the renderer itself is a separate C++ library.

## Local conversion workflow

Read the [usage and ownership disclaimer](DISCLAIMER.md) first. Use your own
lawfully obtained game files and keep all output under ignored `work/`.
Import and generation require new output directories.

```powershell
.\build\Release\AnyPS2.exe import 'C:\path\to\owned-game.iso' work\game
.\build\Release\AnyPS2.exe inspect work\game\boot.elf --flow
```

Boot ELF import and inspection are available. Experimental EE entry-flow conversion
now emits LLVM IR, initialized data and a CMake project. It supports the documented
scalar/control-flow and RAM subset, with native SetupThread, SetupHeap and EndOfHeap
services. Other unsupported syscall boundaries stop explicitly. Most syscall services and the converted EE/IOP connection are unfinished.
An existing local GT4 startup prefix executes through SetupThread and SetupHeap;
its next instruction is not compiled in that older artifact. The converter now includes
broader scalar, packed-integer, floating-point and VU macro-operation families.
See [EE native conversion](docs/ee-native.md) for supported semantics and remaining
boundaries. The latest larger local startup region now emits LLVM, compiles to a
native executable, and runs through semaphore creation, callback registration and
compiled handler execution. A guest-readable view of native registrations lets startup find those handlers.
With a private copied-payload mapping, the latest bounded run executes the
ahead-of-time compiled payload and registers its syscall handlers, then reaches
an unsupported VU microprogram upload in a VIF0 batch after 6,363,631 operations, past
Timer 2 interrupt configuration, thread creation, priority change, native
application settings, further copied-handler registrations and native SIF receiver
initialization and interrupt-safe receive rearm. A native bootstrap command peer now accepts command-buffer and
software-register updates, RPC initialization and unavailable-server bind replies.
Stored command-table pointers and callback arguments passed to registration
functions are now discovered ahead of time. Interrupt-safe semaphore completion
returns to startup. With an explicit disc image, native file-I/O RPC binding and
the service version query complete. Open/read/seek/close and compiled interrupt
completion pass synthetic disc tests; GT4 has not opened or read a file yet.
ELF constructor/destructor tables now supply additional compilation entries.
Native scratchpad storage, Vulkan-backed GS reset and GS mask access are available.
SetGsCrt and nominal display deadlines use a host monotonic clock. Native VIF/GIF
reset and configuration and native VU control setup are available. VIF/GIF submission
configuration validates normal and bounded chain transfers. A native consumer
applies supported VIF configuration batches atomically; VU microprogram conversion,
remaining packet commands,
drawing translation/presentation, further indirect-target discovery
and game-supplied IOP module integration remain unfinished. DMA completion delivery now shares the native interrupt
dispatcher and is verified with a compiled packet-to-handler test. Address-translation
routines compile to native requests. Complete indexed layout declarations can be
lowered to validation of existing native RAM and scratchpad allocations; additional
aliases and layout readback remain unsupported. No runtime TLB provider is connected. Native
thread lifecycle and cooperative priority switching are implemented, with worker
execution and return covered by compiled synthetic tests. Timer 3 alias
registration, public INTC handler chains and native delivery of externally raised
events are implemented. Native polling timers are
connected to compare/overflow events; gated timing and reset-on-compare remain
unsupported. Callback discovery supports simple
wrappers and initialized tables; more dynamic cases remain incomplete.
Visible boot and gameplay are not established.
CFC2/CTC2 compile to native vector-service calls; the production vector owner is
not connected, so executing these calls currently stops explicitly.

```powershell
.\build\Release\AnyPS2.exe recompile work\game\boot.elf work\ee-project
.\build\Release\AnyPS2.exe build work\ee-project .
.\work\ee-project\build\Release\ps2-native.exe work\ee-project\program.data 10000000 --disc "path/to/your-game.iso"
```

The last command reports a diagnostic stop, not a successful game boot. Unsupported
reachable instructions reject conversion before creating output. Missing operation families
are grouped with counts and their first locations to support batch implementation. Existing output
directories are never overwritten. Keep all commercial input and generated output
under ignored `work/`.

## Standalone IOP conversion

An initial standalone IOP converter also generates a CMake project for a supported
IRX module or an explicitly ordered module bundle. LLVM IR is the only output backend.
It compiles ahead of time and links no instruction interpreter,
decoder, JIT or device library. Its current scope is bounded integer/control flow
and byte, halfword and word RAM access, including paired unaligned word loads/stores.
Bounded switch tables support register jumps and calls, including an index shift
in the guard's delay slot. Bundle failures identify each failing module before
emitting output. The `sysclib` major-version-1 `memcpy`,
`memmove` and `memset` imports become checked native memory operations. `sysmem`
major-version-1 allocation, release and memory-query imports use a host-owned
allocator with reserved module/stack regions. Native heaplib reuses that allocator
for fixed/growing subheaps, eight-byte allocations, frees and capacity queries.
Guest allocator internals are not exposed. `loadcore` registration/release
imports maintain a native registry of validated, already-compiled exports, and
library-table queries use that registry. Non-auto-link registration exposes
compiled export tables to queries while excluding them from import linking. Reboot-handling mode updates retain
validated export metadata; a native reboot lifecycle is not implemented. Boot-mode queries read explicitly supplied
native boot records; the standalone probe starts with an empty boot context.
Startup callbacks are queued during module entry execution and drained by priority;
later registrations execute compiled callbacks immediately.
Native sysclib character classification preserves fixed ASCII flags without host
locale dependencies; undefined non-ASCII entries stop explicitly. Native prnt
shares the bounded formatter and delivers output through compiled callbacks,
including flush/reset controls. Native strcmp/strlen/strncmp use bounded guest
byte reads; strchr/index and strrchr/rindex share the same bounds and preserve
guest pointer aliases. Native strcpy/strncpy validate complete writes and preserve
termination/padding rules. Native bzero and _wmemset share destination validation
and preserve byte/word fill semantics. Native sprintf shares formatting and
validates output before writing. Native strcat reuses the bounded string-copy
path. Native strtol uses explicit 32-bit parsing and checked end-pointer writes.
Unsupported format conversions stop explicitly.
Native AddDrv/DelDrv run compiled lifecycle routines and maintain bounded native
registrations. Failed initialization rolls back; teardown runs before removal.
Compiled driver open/read/write/seek/close calls use native handle ownership and heap-backed
public file records. They share a collision-free descriptor pool with the ISO
mount. Legacy compiled drivers also support dopen/dread/dclose with independent cursors.
Extended compiled drivers support lseek64 with two-register offsets and results.
Extended directory records are converted to the legacy caller layout through
aligned temporary storage. Compiled ioctl/ioctl2 callbacks provide device-command
behavior with bounded ioctl2 buffers. Path-based remove/mkdir/rmdir also invoke
compiled operations with temporary file records. Extended devctl dispatch uses
the same ownership and buffer checks. Extended chdir/sync/umount dispatch also
uses compiled callbacks. Extended readlink forwards bounded output buffers and
returns the driver byte count. Rename/symlink route validated same-device paths
to compiled callbacks. GetDevType resolves compiled-driver and native ISO handles
through their shared descriptor ownership. Mount dispatch validates arguments and
invokes the compiled extended driver; other unsupported operations stop explicitly.
Native CpuInvokeInKmode calls compiled functions with a saved continuation and
held event delivery. Nested and interrupt-context invocation remain unsupported.
Native interrupt services register compiled handlers, retain masked events, and
save/restore continuations during cooperative callback delivery. Device event
producers and preemptive thread scheduling are not yet connected. A cooperative
native scheduler now retains explicitly admitted continuations across SleepThread
and foreground/interrupt wakeups, reporting idle waits without consuming execution
budget. Compiled CreateThread/StartThread/DeleteThread calls manage native workers
and owned stacks. StartThreadArgs copies bounded argument blocks onto the worker
stack and supplies their byte count and pointer. ExitThread retains a dormant worker; ExitDeleteThread releases
it after the exit call retires. Ready selection uses priority at cooperative switches. Native alarms use a
monotonic host clock and compiled interrupt callbacks; asynchronous thread
preemption remains unfinished. Native event
flags support creation/deletion, signalling, keep-mask clearing, polling, status
queries and scheduler-backed waits. Unsatisfied waits suspend their compiled
continuations; signals and event deletion resume them with the appropriate result.
GetSystemStatusFlag returns a shared native event initialized from supplied boot
context; an empty context does not invent status bits.
Native SIF transfer services can copy validated aligned batches into an explicitly
connected EE data arena and deliver compiled completion callbacks. The standalone
IOP probe has no converted EE endpoint yet, so these transfers stop there.
Native SIF control supports explicit peer readiness, status acknowledgements and
command-buffer addresses. Validated inbound payloads notify compiled SIF1 handlers
and require receive rearming before another delivery.
Native SIF command registration supports user/system handler tables and checked
two-argument callback delivery from explicit peer packets. Outbound command and
extra-data sends require a connected native peer endpoint and share raw-transfer
completion callbacks, tokens and shutdown. Command initialization waits on the
shared status event for an explicit peer acknowledgement. Inbound packets share
one interrupt-gated native queue for control and registered handlers; RPC and the
converted EE peer remain unfinished. The standalone host supplies no peer.
Native ChangeThreadPriority updates scheduler ordering and yields to eligible
ready workers at the service boundary; it preserves FIFO order among equal
priorities. ReferThreadStatus reports native lifecycle state, creation metadata,
priorities and event/sleep/delay waits. DelayThread uses a native host-clock
deadline, and the standalone host sleeps until the next alarm or delayed worker.
Asynchronous preemption remains unsupported.
Native semaphores support creation, deletion, waiting, polling, signalling and
status queries with FIFO or current-priority waiter selection. Priority inheritance
is explicitly unsupported. Events and semaphores share native scheduler waits.
Native message boxes support queued/direct delivery, receive/poll, deletion and
status, with FIFO or priority ordering for messages and waiting workers.
Native GetOtherData reads through an explicitly connected peer-memory endpoint,
validates and snapshots bytes, and completes at the native service boundary.
Both blocking and NOWAIT modes may complete immediately; absent peers stop.
Native RPC queue/server registration and removal preserve public descriptor
fields and require compiled handlers. Server-side bind requests can return a
validated service description through an explicit peer. Incoming calls can deliver
payloads, wake a registered worker and be retrieved in queue order. ExecRequest
runs compiled handlers and delivers command-notified or polling-mode replies
without rerunning a handler on delivery retries. RPC initialization waits for command setup, publishes readiness once and waits
for explicit peer acknowledgement. The native RPC loop drains queued requests and sleeps through the shared scheduler
until another request arrives. Client operations remain unfinished.
A shared bounded native printf/Kprintf service supports common 32-bit integer, string and
character diagnostics through a host-provided sink; unsupported formats stop
without partial output. Read-only `ioman` open/read/seek/close imports use a shared ISO9660 file backend.
Native getstat returns disc-derived file/root metadata, including size, read-only
type flags and the ISO recording date, without allocating a file handle.
Native dopen/dread/dclose enumerate ISO directories with independent cursors,
shared descriptor limits and the same metadata encoding as getstat.
The ISO mount rejects writes and path mutations with filesystem errors; it never
modifies the supplied disc image or routes those paths to the host filesystem.
Native lseek64 supports checked 64-bit cursor positions and the O32 two-register
return convention; the legacy seek retains its signed 32-bit range.
Native disc ioctl/ioctl2 validate descriptors and reject unknown or non-streaming
controls with I/O errors. Known hardware ioctl commands remain unsupported.
Mount your image at execution time with `iop-native --disc <image.iso>`; guest
paths are restricted to disc files and never resolve to arbitrary host files.
Constant CreateThread descriptor addresses can seed worker discovery from
initialized bundle data, including cross-module descriptors and address aliases.
The converter can compile a bundle of disjoint modules and bind imports
to their compiled exports, with explicit unlink/relink and release protection.
The bundle CLI invokes startup entries in input order with shared native services;
automated disc module discovery and loading are unfinished.
Other imports and unresolved indirect
calls/jumps are rejected during conversion; complete module/service conversion
is unfinished. This separate probe is not yet integrated into GT4 startup.

```powershell
.\build\Release\ps2-iop-recompile.exe 'C:\path\to\module.irx' work\iop-native 1000
cmake -S work\iop-native -B work\iop-native\build -A x64 "-DANYPS2_ROOT=$((Get-Location).Path)"
cmake --build work\iop-native\build --config Release --target iop-native
.\work\iop-native\build\Release\iop-native.exe
```

To package multiple modules, specify each relocation base and path in startup order:

```powershell
.\build\Release\ps2-iop-recompile.exe --bundle work\iop-bundle `
    1000 'C:\path\to\provider.irx' 3000 'C:\path\to\client.irx'
```

Use the same CMake commands with `work\iop-bundle` as the project directory.
The bases must leave disjoint module ranges below the host invocation stack.
Prefix a base with `sealed@` to compile that module for a later LoadStartModule
without copying it into the startup image. Its relocated bytes are written to
`sealed.data`. `sealed@` cannot be combined with `romdrv@`.
`modules.txt` records the startup order, entry points and memory extents. All
startup initialized data is loaded before the first entry runs. Startup stops at the first
unsupported operation; entry return values are reported without interpreting them
as PS2 module residency status. This is explicit native startup orchestration,
not automatic GT4 boot discovery.

To inventory recognizable executables on a disc without stopping at the first
unsupported module:

```powershell
.\build\Release\ps2-convert-inventory.exe D:\path\to\disc.iso work\conversion-inventory
```

The tool writes `report.txt` plus LLVM IR for modules the current converter
accepts. Optional `--payloads` uses the same mapping file as `ps2-recompile`.
`--no-compile` skips Clang. Unrecognized files are counted and not decoded.
EE conversion also seeds complete leaf syscall-wrapper candidates found in executable
segments, including wrappers not reached during startup. The report distinguishes
native dispatch, statically recovered registered handlers, missing dispatch and
native memory-lowering gaps. These are static candidates, not a list of calls
proven necessary at runtime; dynamic syscall numbers and unrecovered code remain
explicit gaps.
Machine-code objects are not linked or executed. A completed inventory is not a
boot, a frame, or gameplay. Keep the output under ignored `work/`.

LLVM output contains `generated.ll`, metadata in `module_metadata.cpp`, and initialized
`module.data`. The initial LLVM build requires Clang on PATH and Windows x86-64
with MSVC. CMake compiles the IR with Clang at `-O2` and links the native object.
Other host targets are not yet validated or enabled by the generated project.
It needs AnyPS2 headers, native disc sources and its probe host source at build time, and `module.data`
beside the executable at run time. It does not load the original IRX or decode
the data as instructions. Keep all converted commercial content under `work/`.

## Components

| Program or library | Responsibility |
| --- | --- |
| `AnyPS2` | Front door for the separate command-line tools |
| `ps2-import` | Read ISO9660, resolve SYSTEM.CNF, validate and extract boot ELF |
| `ps2-inspect` | Inspect ELF metadata and discovered control flow |
| `ps2-recompile` | Generate an experimental EE LLVM entry-flow project from a boot ELF |
| `ps2-iop-recompile`, `nt_iop_codegen` | Convert supported IOP modules into LLVM projects |
| `ps2-convert-inventory` | Inventory recognizable disc executables, convert independent modules, and group missing APIs |
| `ps2-build` | Invoke CMake and compiler with explicit process arguments |
| `nt_binary`, `nt_disc`, `nt_elf` | Bounded input parsing |
| `nt_r5900`, `nt_analysis` | Decode and discover EE code |
| `nt_runtime`, `nt_hardware` | Guest state, RAM, and partial device models |
| `nt_kernel` | BIOS-free startup services and guest-installed syscall handlers |
| `nt_fileio` | Read-only disc file handles and legacy FILEIO RPC operations |
| `nt_process` | Shared process execution |

## Current limits

The runtime supports a subset of EE integer/MMI operations, delay slots, memory
transfers, limited floating-point behavior, startup thread/heap services,
thread creation/start/exit/deletion, normal-context termination/suspension/resume,
wait release and priority scheduling at syscall boundaries,
thread status queries, volatile OSD configuration, FIFO semaphore blocking/wakeup
when another thread is runnable,
guest syscall registration and callbacks,
interrupt registration/masks, nongated timer counting, and partial SIF0/SIF1 DMA.
The BIOS-free IOP endpoint provides RAM, command initialization, software registers,
RPC initialization replies, and unsuccessful binds for unregistered RPC servers.
SetSyscall is no
longer the initial blocker. Unknown behavior stops explicitly.

Graphics, audio, IOP/VU microprogram execution, complete DMA and disc services,
remaining RPC services, and timer-driven thread preemption
remain unfinished. The timer model charges
one modeled EE cycle per retired instruction and bounded device steps while all
threads sleep; it is a diagnostic approximation.
Full floating-point arithmetic and general self-modifying code are unsupported.

SIF payload movement, completion, and registered DMAC/INTC handler delivery/return are
implemented, including preservation of the interrupted CPU context. Default
uncached user RAM aliases share memory with DMA. The IOP endpoint is a small
C++ service implementation, not a complete IOP emulator. DMA completion does not
imply a successful RPC operation. See [SIF implementation](docs/sif.md).
The local GT4 test passes timer interrupt delivery and now binds to a mounted
file-I/O service. With the disc mounted it passes GS CSR initialization and
GsPutIMR, SetGsCrt, VIF0/VIF1 reset, VU control setup, GIF/DMA initialization and
raw FPR transfers, vector addition/subtraction, masked moves, lane rotation,
VU integer-register setup, scalar comparisons and finite scalar multiplication,
passes initial VIF0 DMA transfers, thread sleep/wakeup and CP0 performance-counter
initialization, GS image uploads, scalar arithmetic and both conversion directions,
scalar minimum/maximum and packed interleaving, VIF buffer-address setup, FLUSH
and DIRECT, then passes solid and filtered indexed sprite drawing. The current
bounded run passes CD/DVD initialization and SifStopDma, then reaches the
disc-supplied IOP module-manager startup boundary. The reboot path parses the
disc archive, executes its memory bootstrap and allocates/loads/binds the module
manager in a private IOP context, then executes its entry with boot parameters. Full
module-manager/service startup and completed disc-backed reset remain unfinished.
The GS component supports
privileged register storage, CSR reset/acknowledgement, IMR masks, and
diagnostic interlaced NTSC/PAL scanout events. GIF paths 2/3 support packed A_D
configuration writes and REGLIST packets, including SIGNAL/LABEL/FINISH effects.
Host uploads support PSMCT32/PSMCT24 pixels in swizzled local memory. Other image
formats/directions and general rendering remain unfinished. Solid sprites support
PSMCT32/PSMCT24 framebuffers, scissor, XYOFFSET, framebuffer masks and FBA.
Vulkan texture sampling supports nearest and bilinear PSMCT32/PSMCT24 sprites with fixed UVs,
MODULATE/DECAL and clamp modes. PSMT4 sampling supports 32/24-bit palettes and
CSM1/CSM2 loading. Other indexed formats, mipmaps, mixed min/mag filters, blending and depth
writes remain unsupported. No visible game boot is verified.
The shared VIF register model supports idle reset/stop/cancel, error masks and
configuration readback. VIF0/VIF1 DMA supports normal and basic source chains,
configuration commands and microprogram uploads. VU microprogram execution,
UNPACK and graphics packet processing remain unsupported. VU0 macro support includes finite VADD/VSUB with
lane-wise and broadcast operands, VMOVE, VMR32 and raw vector transfers;
extended numeric ranges and pipeline timing are
not implemented.
GIF supports reset, pause and mode configuration. GIF DMA shares normal/source-chain
transport with VIF; unsupported packets stop before consuming their current beat.

The generator uses bounded dispatch functions and instruction guards. Efficient
basic blocks and measured performance are goals; producing a native executable
alone does not establish a speed improvement or game compatibility.

## Ownership and contributions

You must own the games you use and have the necessary rights to use your copies.
Ownership alone does not authorize redistribution of game images, assets,
translated game code, or compiled game executables. AnyPS2 includes no games or
Sony BIOS, firmware, or proprietary SDK material. Do not commit those materials,
including to private repositories. Read the full [disclaimer](DISCLAIMER.md).

The repository-content test checks source candidates and indexed Git content for
recognizable excluded artifacts. It supplements human provenance review; it
cannot determine copyright ownership of arbitrary text.

See [AGENTS.md](AGENTS.md) for engineering and contribution rules,
[architecture](docs/architecture.md) for implementation boundaries, and
[source references](docs/source-references.md) for upstream provenance and notices.


For offline VU conversion investigation, the native probe accepts
`--capture-vu work/new-private-directory`. It saves the first rejected, complete
MPG payload and its unit/destination metadata without accepting the upload.
The destination directory must not already exist. Keep captures private and ignored.
The capture also saves `packet.bin`, the complete rejected submission. Use
`ps2-vu-extract packet.bin work/new-extraction 0` to extract its ordered uploads and
assemble contiguous image regions offline. The extractor stops before launches,
interrupt commands and unknown formats; it never executes uploaded code. Its manifest
records destinations and the extraction boundary. Later writes replace earlier bytes,
and gaps remain separate files.
Run `ps2-vu-inspect work/new-private-directory/upload.bin 0 0` using the captured
unit and destination values to inventory paired instructions. This tool currently
frames input, decodes typed upper arithmetic and supported lower operations, counts encoding families and discovers control-flow candidates from
the supplied destination as an assumed entry; it does not generate LLVM or execute VU
microprograms. EE macro-vector LLVM support does not imply microprogram support.


`ps2-vu-recompile input.bin output.ll unit destination` now emits LLVM for a bounded
VU basic block ending in a branch or E-bit delay pair. Initial native lowering
covers finite add/subtract/multiply and multiply-add/subtract (including accumulator destinations), NOP/ABS,
min/max and scaled conversions above, plus integer arithmetic, literals and bit-preserving
vector transfers below. Synchronized finite Q operations include DIV, SQRT, RSQRT,
WAITQ and upper Q operands. Q reads requiring unsynchronized latency handling stop
explicitly; automatic pipeline visibility is not implemented. Unsupported operation
families are reported together. Native upload/launch binding remains unfinished.

An optional final comma-separated entry list enables multi-block conversion:
`ps2-vu-recompile input.bin output.ll 0 0 0,32`. This emits reachable native functions
and block-table/count accessors in one LLVM module. Entries must come from conversion
analysis; the upload destination is not proof of a launch entry. Indirect targets
need explicit entries, and missing code is reported without emitting a partial module.
The first private GT4 submission contains two adjacent uploads. Its currently
identified structural entries produce 30 native blocks that compile to x86-64.
Quadword loads/stores, including pre-decrement and post-increment forms, now lower
to native memory operations. VU0 accesses to the VU1 register window stop explicitly.
Neither runtime target coverage nor execution of this game upload is proven.

Generated VU modules also expose their source-image identity and native block table.
A reusable binding accepts matching upload batches atomically and permits native
launch only after the complete image has arrived. This path is tested with synthetic
compiled programs. Newly generated EE projects can link separate unit modules with
`ps2-build <project> <source-root> --vu <vu0.ll> --vu1 <vu1.ll>`.
Each unit tracks upload coverage independently. The private startup accepts both
compiled uploads and reaches a later unsupported-system stop. VU launch/state
handoff and selection among multiple images for one unit remain unfinished.

VU integer-memory operations (`ILW`, `ISW`, `ILWR`, `ISWR`) also lower to native
loads/stores. VU1 conversion remains bounded by the supported operation and
scheduling semantics.
`XTOP`/`XITOP` read explicitly supplied native launch inputs; missing inputs stop
execution. `OPMULA`/`OPMSUB` use the shared finite arithmetic path for xyz products.
VU1 also supports synchronized `ERLENG`, `WAITP` and masked `MFP`. Pending P reads
without supported synchronization stop explicitly; automatic P latency remains unfinished.
MAC comparisons and bitwise reads (`FMEQ`, `FMAND`, `FMOR`) are supported when flag
visibility is established. Reads that require additional stall/flag timing stop.
Ordinary VU1 `XGKICK` lowers to a native graphics request boundary with bounded
packet framing. A missing consumer stops execution; Vulkan packet rendering,
and asynchronous transfer behavior remain unfinished. Kick-plus-E supports a final
delay pair without data stores and reports completion only after packet acceptance.
CLIP and its flag operations now lower to native code with conservative visibility
checks. The identified structural entries in the captured VU1 image compile into
392 native blocks. Both unit artifacts link and bind during private startup;
game VU launch execution and visible rendering remain unverified.

VIF1 `MSCAL` can invoke a bound native VU1 program, with TOP/ITOP inputs and
buffer selection. Launches commit only on completion. The initial path accepts
one launch at the end of a configuration batch. Mixed program uploads/launches
remain unsupported. MSCNT can resume a previously completed native launch at a compiled continuation entry.
VIF1 MSCALF and graphics flush commands require an explicit native graphics wait
owner. To compile a known chain of subsequent E-stop entries,
use `ps2-vu-recompile input.bin output.ll 1 0 0 --continuations 2`.
The depth counts additional completed launches, not instruction pairs; missing
continuation code is rejected during generation.
Compiled synthetic programs verify this path. Private GT4 startup now passes
performance-counter configuration and recovered object/switch-table calls, then
accepts split GIF image transfers and schedules native threads through sleep and
wakeup. Disc-backed CD/DVD RPC initialization now succeeds; the current native
run completes the linked IOP reset and PADMAN initialization, then reaches
native graphics initialization. The default Windows profiling replacement uses
`QueryPerformanceCounter`, without administrator access. Cycle and cache profiling
slots accumulate elapsed clock ticks, not PS2 cycles or cache-miss events. The
runtime reports the clock frequency; waits and native services contribute to elapsed
time. `--strict-performance` retains diagnostic stops for unmeasured events.
Hardware profiling remains an optional diagnostic mode, not a startup requirement.
See [native measurement limitations](docs/ee-native.md#native-elapsed-time-profiling).
GT4 now produces its opening copyright/driving-warning image through Vulkan.
The private render-target readback contains actual game-generated text. Display
selection now reaches the same image at the configured 640 x 448 resolution.
The executable opens a Vulkan window during execution and keeps the final image
visible at a diagnostic stop, labelled in its title. The title also reports
presented frames and presentation FPS; the frame counter freezes when execution
stops and does not count repeated inspection redraws. Use `--headless` for automated
runs and `--run-seconds N` for a bounded diagnostic duration. `--capture-stop`
saves separate render-target and selected-display evidence. Native startup now
completes the warning thread and resumes the main thread; its next unsupported
boundary is native address-space initialization. The title screen and gameplay
are not established.

Scalar, V2, masked V3 and V4 uploads in 32-, 16- and 8-bit formats,
plus packed V4-5 data, can populate
native VU1 storage, including wrapped and TOPS-relative destinations. Narrow lanes
support signed/unsigned widening; packed colour lanes expand independently of MODE.
Data and a final MSCAL commit together only when execution completes. Other unpack
formats, input-selected V3 fourth lanes and input-dependent fill lanes remain unsupported. Masked row/column/
preserve filling is supported. Fixed row addition, accumulating/
replacement row modes and skipped destination slots are supported. Masked uploads select
input, row, column or preserved lanes with transactional configuration snapshots.

VIF0 launches can share completed vector, integer-register and flag state with
native EE vector operations. An active owner prevents a new launch; failed work
does not publish state. Both units use compiled functions through independent
submission owners. VCALLMS and VCALLMSR lower through LLVM to the same native VU0
owner, including CMSAR0 entry selection. Compiled EE loads/stores access the same
native VU data storage, with extent and ownership checks. VU instruction storage
is not exposed as writable data.

Compiled VU graphics requests can be staged as an ordered immutable batch and
accepted by a native consumer only after the whole launch completes. A missing
or rejecting consumer leaves execution uncommitted. A native adapter now translates
supported packed/address-data and register-list packets into Vulkan solid-sprite
commands. RGBA32/RGB24 and PSMT4 host image uploads preserve ordering with draws, and fixed-UV
sprites can sample those textures through Vulkan. PSMT4 indexed sprites support 16-entry RGBA32/RGB24 palettes with ordered and conditional loads.
Sprites also support alpha tests and integer alpha blending.
Perspective textures, other primitive types and display
presentation through this path remain unfinished. Synthetic compiled VU-to-GPU
tests pass; game rendering is not established.

VIF1 DIRECT/DIRECTHL and direct GIF submissions share that same Vulkan adapter.
They require complete bounded packets and serialized execution. Unsupported mixed
vector/graphics batches and split packets stop without partial drawing.

The native VU runtime can route between registered ahead-of-time compiled blocks,
with bounded execution and explicit stops for missing targets or unsupported numeric
inputs. It has no instruction decoder or interpreter fallback. This is validated
with original synthetic programs; it is not yet connected to GT4's VU launches.

To inspect performance-counter dependencies before choosing a native measurement
replacement, run `ps2-inspect <executable.elf> --metrics [window]` (default 128, maximum 4096). This offline report
lists executable-segment candidates, bounded register uses, and candidate memory
accesses in direct callees receiving a measurement-structure pointer. Stored values and
unresolved flow require further analysis; the report does not authorize removing
measurements. Keep commercial-input reports under ignored `work/`.

New generated EE projects retain a linker map and `.audit.txt` beside the native
executable. A post-link check verifies compiled-entry evidence and rejects known
interpreter, decoder and JIT dependencies. The report includes executable/map
hashes and imported DLLs; it is a regression guard, not proof about arbitrary
linked code. Keep commercial-input maps and reports under ignored `work/`.


On Windows, `anyps2-host-metrics` lists the native profiling sources advertised
by the current system. It is a read-only capability probe: it does not install a
driver, configure global counters, start tracing, or provide guest measurements.
Advertised sources alone do not establish a usable native performance provider.

For an unencrypted length-prefixed DEFLATE executable container, `ps2-unpack`
produces a private ELF input for ordinary LLVM conversion:

```powershell
.\build\Release\ps2-unpack.exe --disc 'C:\path\to\owned-game.iso' 'path/to/packed-program' work\secondary.elf
.\build\Release\ps2-recompile.exe work\boot.elf work\native --image work\secondary.elf
```

The importer validates bounded segments and rejects conflicting overlaps; opaque
container metadata is not authenticated. Only the segment containing the entry
point is initially marked executable. Repeat `--image` to compile additional,
disjoint images into the same executable. The compiled loader must populate their
bytes before `ExecPS2` validates and activates them. This does not establish full
code discovery or title-screen compatibility; see [native execution](docs/ee-native.md).
