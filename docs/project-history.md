# Project history and verified milestones

AnyPS2 began as an experiment in converting user-supplied PS2 programs into native
PC software, with Gran Turismo 4 as the first commercial integration target.
Development has been directed by SamG-Coder with AI-assisted implementation and
review using Codex and, during part of development, Grok. AI assistance is not
proof of correctness or provenance: source changes still need review and tests.

## How the architecture developed

- `87a2109` established the C++ toolchain and tested SIF transport. Early hardware
  models and reference execution helped investigate startup behavior.
- `669d91f` made LLVM the sole conversion backend. `e0761cc` introduced EE LLVM
  AOT conversion and native startup services. The current direction is typed IR,
  LLVM machine code and reusable C++ services, not generated C++ game code.
- `62f57fa` added VU compilation; `54ddac1` connected compiled EE/VU execution to
  native Vulkan sprites. Subsequent work added native services, resource loading,
  graphics and stricter discovery. GT4 has rendered its opening warning, but has
  not reached a verified title screen or playable gameplay.
- `f435433` added validated runtime-stop observations as inputs to a subsequent
  offline conversion. `ee47497` stabilized LLVM units for incremental rebuilding.
  Neither change makes unknown code executable at runtime.
- `433dfa1` added original cross-compiled PS2 source tests and external reference
  comparison. These exposed callback-discovery gaps that simple opcode tests missed.
- `9d436c8` established the working homebrew cube milestone described below.

Existing register-state/address adapters and diagnostic hardware models remain
migration work. Their presence is not concealed by the native executable suffix;
the intended ABI uses recovered functions and explicit native module bindings.
Unknown instructions, services and indirect targets must remain explicit failures.

## One working homebrew demo

The locally adapted **glampert/ps2-homebrew cubes demo**, pinned to
[`602441a6877a3136709d6320664340d52e3027a1`](https://github.com/glampert/ps2-homebrew/tree/602441a6877a3136709d6320664340d52e3027a1),
was built into a PS2 ELF, converted to LLVM, compiled to Windows x64 machine code
and run with AnyPS2's native services and Vulkan renderer.

The verified 20-second run displayed an animated grid of checker-textured cubes
and text at 640 x 448, submitted 55,432 triangles, and stopped at the requested
diagnostic deadline. A subsequent controller-enabled run lasted 120 seconds,
submitted 1,003,962 triangles, and ended only at its requested deadline. The user
confirmed camera movement with a physical PS5 DualSense. Native SDL input feeds
the public legacy pad RPC interface; the local source port binds those endpoints
without requesting ROM modules. Vertical look preserves the original demo's
inverted direction. This establishes an interactive homebrew demo, not general
homebrew compatibility or commercial-game playability. Native edge coverage
also differs from GS precision.

The local port updates compiler/assembler compatibility, assembly memory clobbers,
controller failure handling and a public SDK synchronization helper. It is not an
unmodified upstream compatibility test. Source, assets, ELF, generated IR, native
binary and screenshots remain outside Git. The upstream project includes material
with different provenance; its top-level source license is not a blanket asset license.
See [source references](source-references.md#homebrew-integration-candidate) and
[homebrew testing](ps2-test-programs.md#larger-homebrew-tests).

## References and attribution

The main references have been PCSX2 for instruction/device behavior and external
differential tests, PS2SDK for public interfaces, Play! for service behavior, and
other recompilation projects for architectural comparisons. These are acknowledged
in the [pinned provenance ledger](source-references.md), including licenses and
whether material was consulted, adapted or linked. They are not shipped runtime
fallbacks. Commercial inputs were used only for local integration investigation;
the repository contains original synthetic fixtures instead of extracted game code.

## Public-source review

The pre-release review at `9d436c8` scanned all 87 commits reachable through local
Git refs and 1,598 unique historical blobs. It found no binary signatures/NUL-bearing
files, prohibited game/build artifact paths, oversized source blobs, generated-guest
source headers, or private-key/token patterns in that scan. The current index and
working-tree repository-content checks are separate acceptance gates.

These checks detect common publication mistakes, not copyright ownership or every
possible secret. Reference notices and build dependencies were also reviewed. No
history rewrite was required. Local inputs and investigation artifacts remain
ignored; they must not be included in a release upload.
