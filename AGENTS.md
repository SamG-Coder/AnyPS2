# AnyPS2 contributor instructions

## Goal

Build a generic, BIOS-free C++20 toolchain that converts a user-supplied PS2 ISO
into a modern native PC executable through typed IR and LLVM ahead-of-time compilation. Gran Turismo 4
is the first integration target. Preserve AnyPS2 and other user-supplied names;
do not add alternative place names or naming subtitles.

Implement reusable PS2 behavior. Never select runtime behavior by game title,
disc ID, hash, instruction address, or observed GT4 register values. Unsupported
behavior must stop with actionable diagnostics. Do not fake successful services
to advance a trace. Do not require, embed, or reconstruct a proprietary BIOS.

## Native conversion is mandatory

- Convert PS2 program code into typed IR and compile it ahead of time through
  LLVM into the output executable. This includes EE, IOP and VU code and loadable modules discovered
  in the user-supplied input. Generating an emulator wrapper is not conversion.
- The final program must not fetch/decode/interpret PS2 instructions at runtime,
  use a JIT, or fall back to PCSX2 or another emulator. Unsupported code or unknown
  executable content must produce actionable conversion diagnostics.
- Recover functions and generate native service calls. Translate graphics,
  audio, input, file-I/O and synchronization behavior into native implementations;
  use Vulkan for graphics. Do not hide whole-machine emulation behind these APIs.
- Connect recovered modules through native object symbols, linker relocations,
  and explicit native exports/imports. Resolve known calls at conversion/link time;
  use typed native function pointers for genuinely indirect calls and callbacks.
  Compiling an original-address dispatcher or a CPU-state adapter does not by
  itself satisfy this requirement. Existing adapters are migration work, not the
  target ABI. Missing bindings must be reported, never replaced with a runtime
  decoder or a success stub.
- Modernize the implementation for native PC systems during conversion. Identify
  the purpose of console-specific operations and replace them with suitable PC
  APIs, data structures and execution models where behavior can be preserved.
  Vulkan rendering is the model for this approach; apply the same reasoning to
  audio, input, file access, memory management, threading and synchronization.
  Do not reproduce PS2 register banks, DMA machinery or timing loops by default
  when a direct native operation can provide the required observable behavior.
- The deliverable is a native PC executable plus reproducible conversion/build
  artifacts. LLVM is the only backend; do not generate C++ game code. Make modernization changes in the
  converter and reusable native libraries, not by hand-editing generated files.
  Document semantic differences, retain ordering and numeric behavior needed by
  the game, and verify replacements with relevant behavior-level tests. Do not
  substitute approximate behavior silently or claim performance without measuring.
- The existing IOP interpreter and hardware models are diagnostic scaffolding,
  not the target architecture. Do not extend the interpreter as the production
  boot path. Prioritize replacing it with ahead-of-time native code and isolating reference
  execution from shipped output. Never report interpreted startup as converted
  execution or as fulfillment of the user's native-program requirement.
- Acceptance requires compiling and executing generated code for the converted
  subsystems, plus inspecting output dependencies to verify there is no runtime
  instruction interpreter, decoder or JIT fallback. A native file extension,
  successful link or higher instruction count is insufficient evidence.
- Keep generation and compilation as explicit, reproducible stages. Write the
  IR and build configuration to the local output directory, then compile through
  LLVM and link native C++ services to produce the `.exe`. Retain artifacts for
  inspection and rebuilding; all game-code output must use LLVM. Report generation, compilation and
  execution results separately; source emission alone is not completion.

## Clean and reusable C++

- Follow the surrounding naming and layout conventions. Use descriptive types and
  functions; do not introduce a second naming style or mass-rename working APIs.
- Comments explain invariants, ownership, non-obvious numeric behavior or known
  limitations. Remove narration of edits and comments that merely restate code;
  preserve attribution and correctness explanations.
- Keep build orchestration in the root CMake file and test registration in
  `tests/targets.cmake`. Reuse shared I/O and generation helpers instead of adding
  local copies. Extract cohesive responsibilities, not arbitrary line counts.
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
instruction, ELF, and ISO fixtures. Compile and execute generated native code in tests.
Cover malformed inputs, delay slots, state transitions, unsupported paths, and
regressions relevant to each change. Run only tests related to the changed
behavior and its affected integration paths, rather than the full suite by
default. Run CTest in Release. Implement related operation families as a batch
before running their combined milestone tests. Do not repeat integration tests
after each small opcode addition. Build during development when useful, then run
the affected tests after the coherent batch; rerun only for fixes or new concerns.
Honor any testing delay requested by the user.
Keep visible presentation tests disabled during ordinary development. Only run
window-opening tests when explicitly requested; opening a requested demo is separate.

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
Document stable commands, contracts, current limitations and provenance. Do not
create per-operation progress notes, repeated test logs or speculative design
documents. Keep transient investigation in ignored work/ and use commit messages
for change history. Remove obsolete or redundant documentation when replacing it.
Keep one authoritative home for each contract. The source-reference ledger records
one entry per reference with its pin, purpose, license and adaptations; it is not
a second change log. Technical-debt entries must identify an existing code boundary,
its consequence and a concrete completion condition, and be removed when resolved.
