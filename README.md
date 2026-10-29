# AnyPS2

AnyPS2 is a generic, BIOS-free C++20 toolchain with the goal of converting a
user-supplied PS2 ISO into **generated C++ compiled into a native PC executable**.
Gran Turismo 4 is the first integration target.

The prototype imports a boot executable, discovers supported EE code, generates
C++, and builds a Windows executable. Generated instructions execute as native
host code against shared C++ runtime services. **GT4 does not yet boot to a visible
screen or run as a playable game.**

## Pipeline

```text
User-owned PS2 ISO
  -> import and validate boot ELF
  -> discover code and translate supported instructions to C++
  -> compile generated code with shared BIOS-free runtime
  -> native PC executable and local game data
```

The current executable is a diagnostic startup probe. It still requires its
matching sibling `boot.elf`. PS2 hardware compatibility remains necessary. PCSX2
is an open-source hardware reference; it is not currently linked or bundled.

## Build and test

Requires CMake 3.24 or later and a C++20 compiler. The tested platform is Windows
x64 with Visual Studio's C++ workload. Install the Vulkan SDK (including `glslc`
and validation layers) and a Vulkan-capable graphics driver. GS solid-sprite
drawing now uses Vulkan compute; there is no silent CPU rendering fallback.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Tests use synthetic fixtures, including generated C++ that is compiled and
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
.\build\Release\AnyPS2.exe recompile work\game\boot.elf work\game-native
.\build\Release\AnyPS2.exe build work\game-native (Get-Location).Path
```

The result is `work/game-native/build/Release/ps2-native.exe`. Generation writes
`generated.cpp`, `analysis.txt`, a matching `boot.elf`, and a CMake project.
Indirect targets and copied code can require additional observed entry points and
a local memory capture. These artifacts must never be committed.
For disc-backed file I/O, pass `--disc=IMAGE.iso` to the generated probe after its
ELF and instruction-budget arguments. Local CTest integration accepts the same
image through `ANYPS2_TEST_DISC`; see [file-I/O service](docs/fileio.md).

## Components

| Program or library | Responsibility |
| --- | --- |
| `AnyPS2` | Front door for the separate command-line tools |
| `ps2-import` | Read ISO9660, resolve SYSTEM.CNF, validate and extract boot ELF |
| `ps2-inspect` | Inspect ELF metadata and discovered control flow |
| `ps2-recompile` | Emit C++ and a CMake project from supported code |
| `ps2-build` | Invoke CMake and compiler with explicit process arguments |
| `nt_binary`, `nt_disc`, `nt_elf` | Bounded input parsing |
| `nt_r5900`, `nt_analysis`, `nt_codegen` | Decode, discover, and generate code |
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
