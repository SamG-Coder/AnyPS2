# AnyPS2

**Convert PlayStation 2 games into native PC executables — without a PS2 emulator at runtime.**

AnyPS2 is an experimental, BIOS-free C++20 toolchain for converting user-owned PS2 games to native Windows x64 programs. It uses **ahead-of-time LLVM compilation** for PS2 game code and native PC services for graphics, audio, input, files and other console-specific operations.

**Gran Turismo 4 is the first integration target.** The long-term goal is a reusable conversion pipeline, not a one-off GT4 port.

> **Work in progress:** AnyPS2 is not a general-purpose PS2 game launcher. GT4 has rendered its opening warning through Vulkan and progressed into subsequent graphics/startup work, but it has **not reached the title screen or playable gameplay**.

## How it works

```text
User-owned PS2 disc image
        |
        v
Import game files, boot ELF and modules
        |
        v
Discover EE / IOP / VU code and control flow
        |
        v
Translate supported code to AnyPS2 IR -> LLVM
        |
        v
Compile ahead of time and link native PC services
        |
        v
Native executable + local game data
```

The intended output runs compiled native machine code. It must **not** interpret PS2 instructions, JIT-compile them at runtime, or fall back to PCSX2 or another emulator. Unsupported code and services should produce explicit diagnostics rather than silently pretending to work.

Native services aim to reproduce the *observable behavior* of console operations using PC systems where practical. Vulkan is the graphics backend. Matching PS2 behavior, ordering and numeric details remains essential.

## Current progress

| Area | Current state |
| --- | --- |
| Disc import | ISO9660 import, `SYSTEM.CNF` boot discovery, ELF inspection and module inventory |
| EE (Emotion Engine) | Experimental ahead-of-time LLVM conversion of a growing R5900 instruction/control-flow subset; native startup services and compiled callbacks |
| IOP | Standalone LLVM conversion for supported IRX code and a growing set of native kernel, file, thread, RPC and device services; full integration remains incomplete |
| VU / VIF / GIF | Bounded vector-code conversion, native vector services and supported packet paths; incomplete game integration and unsupported operations remain |
| Graphics | Vulkan sprite, texture and triangle paths, including synthetic compiled VU-to-GPU tests; not a complete PS2 Graphics Synthesizer replacement |
| Audio | Startup/service work exists, but native sample-voice playback is not yet implemented |
| Code discovery | Direct/indirect call and callback analysis, with `call-sites.tsv` and `pointer-references.tsv` reports for unresolved coverage |
| Game compatibility | **GT4 is not playable.** No general game-compatibility claim |

### Gran Turismo 4 milestone

GT4 has displayed its opening warning using Vulkan. More recent local startup work has also reached triangle rendering. The latest documented observed boundary is a LIBSD `KON` sample-voice start, for which native playback is still missing.

This is **real progress in the conversion pipeline**, but not proof of a complete boot, correct gameplay, or improved performance. Some subsystems are validated with synthetic programs and are not yet proven with GT4 itself.

## What remains

- Complete EE function and indirect-call discovery, including dynamically constructed pointers.
- Finish native IOP and VU integration and missing console-service behavior.
- Implement the remaining audio, rendering, disc, DMA and synchronization paths needed by games.
- Verify correctness against PS2 behavior and eliminate unsupported startup boundaries.
- Reach GT4's title screen, then gameplay, before making compatibility or performance claims.

The current analysis intentionally reports uncertain pointer references instead of assuming data is executable code. See [EE native conversion](docs/ee-native.md) and [testing](docs/testing.md) for the technical details and validation approach.

## Build

**Tested development environment:** Windows x64, Visual Studio C++ tools, CMake 3.24+, Vulkan SDK (including `glslc` and validation layers), and a Vulkan-capable driver with `shaderInt64`.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Initial configuration downloads the pinned zlib importer dependency. Tests use synthetic fixtures; passing them does not establish compatibility with a commercial game.

## Try a local conversion

Use your **own lawfully obtained game files**. Keep commercial inputs and generated output under the ignored `work/` directory. See [DISCLAIMER.md](DISCLAIMER.md) before use.

```powershell
.\build\Release\AnyPS2.exe convert 'C:\path\to\owned-game.iso' --jobs 4
```

This runs the experimental conversion/build workflow and produces a build plan identifying converted targets and gaps. It does **not** guarantee a runnable or playable game.

For separate import, inspection and EE experimentation:

```powershell
.\build\Release\AnyPS2.exe import 'C:\path\to\owned-game.iso' work\game
.\build\Release\AnyPS2.exe inspect work\game\boot.elf --flow
.\build\Release\AnyPS2.exe recompile work\game\boot.elf work\ee-project
```

Generated output directories must be new. Converted commercial game data is not distributed with the project.

## Technical documentation

- [EE native conversion](docs/ee-native.md) — R5900 conversion, LLVM generation, memory, call discovery and limits
- [Testing](docs/testing.md) — synthetic fixtures and opt-in local game validation
- [Batch conversion](docs/batch-conversion.md) — conversion workflow, LLVM build and performance measurements
- [SIF implementation](docs/sif.md) — native EE/IOP communication work
- [Usage and ownership disclaimer](DISCLAIMER.md) — game-file requirements

## Project principles

1. **Native ahead-of-time compilation**, not instruction interpretation or JIT execution.
2. **No bundled PS2 BIOS or commercial game assets.**
3. **Explicit failures** for unsupported code or unimplemented services.
4. **Behavior first:** rendering a frame or compiling a binary is not the same as running a game correctly.
5. **Reusable conversion:** build infrastructure that can eventually work beyond GT4.

AnyPS2 is experimental software under active development. The source and technical documentation describe implemented subsets; they are not promises of full PS2 compatibility.
