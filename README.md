# AnyPS2

**Convert PlayStation 2 games into native PC executables — without a PS2 emulator at runtime.**

AnyPS2 is an experimental, BIOS-free C++20 toolchain for converting user-owned PS2 games to native Windows x64 programs. It uses **ahead-of-time LLVM compilation** for PS2 game code and native PC services for graphics, audio, input, files and other console-specific operations.

**Gran Turismo 4 is the first integration target.** The long-term goal is a reusable conversion pipeline, not a one-off GT4 port.

> **Work in progress:** AnyPS2 is not a general-purpose PS2 game launcher. GT4 reaches its menus and races in local runs through Vulkan, but it is incomplete, unexplained stops remain, and it does not hold 60 fps. No other commercial game has been shown to work.

**Homebrew milestones:** locally adapted Cubes, Console/Input, Audio/Sound and
Dungeon demos from [glampert/ps2-homebrew](https://github.com/glampert/ps2-homebrew)
run through LLVM AOT and native services. The user verified camera control, button
input, vibration and audio; Dungeon renders its 3D scene but performance work remains.
PS2SDK Teapot also renders through Vulkan. These are adapted sample milestones,
not commercial-game compatibility claims. See [results and limits](docs/project-history.md).

## License and provenance

Original AnyPS2 code is [MIT licensed](LICENSE). Third-party components retain
[their own licenses and notices](docs/licensing.md). The repository distributes
source and original synthetic tests, not games, BIOS/firmware, proprietary SDKs,
extracted assets or converted commercial executables. Reference use is recorded
in the [source provenance ledger](docs/source-references.md).

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
| Graphics | Vulkan sprite, texture and triangle paths, resolution scaling and full screen; not a complete PS2 Graphics Synthesizer replacement |
| Audio | Native audsrv mono ADPCM sample playback, volume and looping through XAudio2; broader LIBSD/game audio and streaming remain incomplete |
| Code discovery | Direct/indirect call and callback analysis; verification mode reports remaining flow and callback-binding gaps before LLVM generation or compilation |
| Game compatibility | **GT4 reaches menus and races in local runs**, with known stops and performance limits. No general game-compatibility claim |

## Build

**Tested development environment:** Windows x64, Visual Studio C++ tools, CMake 3.24+, Vulkan SDK (including `glslc` and validation layers), and a Vulkan-capable driver with `shaderInt64`.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release -N
```

Converted VU code targets `x86-64-v3` (AVX2) by default, and converted programs
check the CPU at startup. For older x86-64 processors, configure with
`-DANYPS2_NATIVE_ISA=x86-64` (or `x86-64-v2`; `x86-64-v4` selects AVX-512).

Select related Release tests from the listing; see [testing](docs/testing.md).
Visible presentation tests are opt-in and disabled by default.
Initial configuration downloads the pinned zlib importer dependency. Tests use synthetic fixtures; passing them does not establish compatibility with a commercial game.

## Try a local conversion

Use your **own lawfully obtained game files**. Keep commercial inputs and generated output under the ignored `work/` directory. See [DISCLAIMER.md](DISCLAIMER.md) before use.

```powershell
.\build\Release\AnyPS2.exe convert 'C:\path\to\owned-game.iso' work\game-native --jobs 4
```

This runs the experimental conversion and build. It does **not** guarantee a runnable or playable game, and converted commercial game data is not distributed with the project. Output directories must be new.

To run the result:

```powershell
.\work\game-native\bin\ps2-native.exe .\work\game-native\bin\program.data --disc 'C:\path\to\owned-game.iso' --fullscreen
```

The [command reference](docs/commands.md) covers every program and option.

## Technical documentation

**Using AnyPS2**

- [Command reference](docs/commands.md): every program and option
- [Batch conversion](docs/batch-conversion.md): conversion output, compiler settings and limits
- [Flow verification](docs/flow-verification.md): check discovery gaps before a native build
- [Testing](docs/testing.md): choosing tests and validating local games
- [Original PS2 test programs](docs/ps2-test-programs.md): cross-compiled test cases
- [Usage and ownership disclaimer](DISCLAIMER.md): game-file requirements

**How it works**

- [Architecture](docs/architecture.md): conversion boundaries and native services
- [EE native conversion](docs/ee-native.md): R5900 conversion, memory and call discovery
- [File I/O](docs/fileio.md): disc-backed file RPC
- [Render modernization](docs/render-modernization.md): the renderer and its direction
- [Loop lifting](docs/render-loop-lifting.md): lifting VU loops to native kernels
- [Technical debt](docs/technical-debt.md): maintenance boundaries and completion criteria

**Project**

- [Contributing](CONTRIBUTING.md): contribution and publication rules
- [Project history](docs/project-history.md): development path, evidence and limits
- [Licensing](docs/licensing.md) and [source references](docs/source-references.md): licenses and provenance
