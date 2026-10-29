# AnyPS2 contributor instructions

## Goal

Build a generic, BIOS-free C++20 toolchain that converts a user-supplied PS2 ISO
into generated C++ and compiles it into a native PC executable. Gran Turismo 4
is the first integration target. Preserve AnyPS2 and other user-supplied names;
do not add alternative place names or naming subtitles.

Implement reusable PS2 behavior. Never select runtime behavior by game title,
disc ID, hash, instruction address, or observed GT4 register values. Unsupported
behavior must stop with actionable diagnostics. Do not fake successful services
to advance a trace. Do not require, embed, or reconstruct a proprietary BIOS.

## Clean and reusable C++

- Product code is C++20. Programs own CLI handling; libraries own reusable logic.
  Keep parsing, decoding, analysis, generation, runtime, hardware, kernel services,
  and process execution separate.
- Reuse existing helpers. Prefer cohesive functions, explicit ownership, bounded
  inputs, and typed interfaces. Avoid duplicate opcode semantics and game hacks.
- Generated and AI-written code must meet the same standards as handwritten code.
  Fix generators rather than generated files. Bound generated function sizes.
- Preserve guest widths, overflow, lane updates, alignment, delay slots, and
  exception state without C++ undefined behavior. Stop on unsupported semantics.
- Target efficient basic blocks and direct control flow. Reduce dispatch, repeated
  guards, and virtual calls only when correctness is preserved. Share slow paths.
  Measure Release builds before claiming performance improvements.
- Libraries must not terminate the process or write directly to the console.

## Tests are acceptance gates

Use automated tests for major behavior changes and integration milestones, not
every small edit or helper. Prefer end-to-end subsystem regressions over tests
that mirror implementation details. A build, manual trace, or larger instruction
count does not establish correctness or a game boot. Commit only original synthetic
instruction, ELF, and ISO fixtures. Compile and execute generated C++ in tests.
Cover malformed inputs, delay slots, state transitions, unsupported paths, and
regressions relevant to each change. Run CTest in Release.

Commercial-game integration tests must be opt-in, take paths from configuration,
and keep inputs and outputs ignored. Distinguish startup progress, visible boot,
menus, and playable gameplay. An expected diagnostic stop is not a game boot.
Do not describe instruction-based timing as accurate PS2 timing.

## Repository content and provenance

Read DISCLAIMER.md. Never commit Sony or other publishers' proprietary material:
BIOS/firmware, keys, games, extracted code/modules, assets, audio, textures, saves,
memory dumps, traces containing game code, translated commercial game C++, or
compiled game executables. Keep local investigation under ignored work/. Never
force-add these artifacts, even to a private repository. Do not copy proprietary
SDKs, leaked source, BIOS disassemblies, or commercial instruction blobs into
implementation, comments, documentation, or tests.

Use public interfaces and appropriately licensed open-source references. Record
pinned references and retain required notices in docs/source-references.md before
adapting code. Do not assume upstream licenses can be replaced. Keep reference
checkouts under ignored third_party/.

Run the repository-content test before staging and again after staging. Review
the staged diff and filenames before committing. The automated guard cannot prove
copyright ownership; review provenance as well. A passing build alone is not
authorization to commit or publish.

## Documentation

Keep README.md focused on the goal, build, usage, limitations, and links. Keep
architecture and test details in dedicated documents. Avoid personal absolute
paths, copied logs, stale claims, and duplicated policy. Update documentation
when observable behavior or supported scope changes.
