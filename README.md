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
x64 with Visual Studio's C++ workload.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Tests use synthetic fixtures, including generated C++ that is compiled and
executed. See [testing](docs/testing.md) for opt-in local game validation.

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

Graphics, audio, IOP/VU instruction execution, complete DMA and disc services,
RPC service implementations, idle waits, timer-driven preemption, and interrupt
handler delivery remain unfinished. The timer model charges
one modeled EE cycle per retired instruction; it is a diagnostic approximation.
Full floating-point arithmetic and general self-modifying code are unsupported.

SIF payload movement and completion are implemented; the IOP endpoint is a small
C++ service implementation, not a complete IOP emulator. DMA completion does not
imply a successful RPC operation. See [SIF implementation](docs/sif.md).

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
