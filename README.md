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
**GT4 does not yet boot to a visible screen or run as a playable game.**
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

Requires CMake 3.24 or later and a C++20 compiler. The tested platform is Windows
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

Boot ELF import and inspection are available. EE LLVM conversion is not yet
implemented: `AnyPS2 recompile` reports that limitation without producing output.
Use the standalone IOP workflow below for supported native module conversion.

## Standalone IOP conversion

An initial standalone IOP converter also generates a CMake project for a supported
IRX module or an explicitly ordered module bundle. LLVM IR is the only output backend.
It compiles ahead of time and links no instruction interpreter,
decoder, JIT or device library. Its current scope is bounded integer/control flow
and aligned byte, halfword and word RAM access. The `sysclib` major-version-1 `memcpy`,
`memmove` and `memset` imports become checked native memory operations. `sysmem`
major-version-1 allocation, release and memory-query imports use a host-owned
allocator with reserved module/stack regions. `loadcore` registration/release
imports maintain a native registry of validated, already-compiled exports, and
library-table queries use that registry. Boot-mode queries read explicitly supplied
native boot records; the standalone probe starts with an empty boot context.
Native AddDrv/DelDrv run compiled lifecycle routines and maintain bounded native
registrations. Failed initialization rolls back; teardown runs before removal.
Compiled driver open/read/write/seek/close calls use native handle ownership and heap-backed
public file records. They share a collision-free descriptor pool with the ISO
mount. Legacy compiled drivers also support dopen/dread/dclose with independent cursors.
Extended compiled drivers support lseek64 with two-register offsets and results.
Extended directory records are converted to the legacy caller layout through
aligned temporary storage; other unsupported operations stop explicitly.
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
`modules.txt` records the startup order, entry points and memory extents. All
initialized data is loaded before the first entry runs. Startup stops at the first
unsupported operation; entry return values are reported without interpreting them
as PS2 module residency status. This is explicit native startup orchestration,
not automatic GT4 boot discovery.

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
| `ps2-recompile` | Report that EE LLVM conversion is not yet implemented |
| `ps2-iop-recompile`, `nt_iop_codegen` | Convert supported IOP modules into LLVM projects |
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
thread creation/start/exit/deletion and priority scheduling at syscall boundaries,
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
