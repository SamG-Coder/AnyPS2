# EE LLVM native conversion

The EE converter discovers a bounded set of R5900 instructions, emits LLVM IR and
compiles it ahead of time. Unsupported instructions and missing compiled targets
remain explicit failures. There is no runtime fetch/decode, JIT or emulator fallback.

## Workflow

Keep user-owned inputs and generated output under ignored `work/`. Output project
directories must be new. Use the same payload mappings and secondary images for
verification and generation.

```powershell
.\build\Release\ps2-recompile.exe work\game\boot.elf work\flow-report --verify-only
.\build\Release\AnyPS2.exe recompile work\game\boot.elf work\ee-project
```

For ELF images with declared EE instruction sections, use `ps2-recompile
<input.elf> <output> --whole-code` (also supported with `--verify-only`). This
selects every aligned word in those sections without entry-point crawling,
callback-pattern discovery or runtime observations. `.vutext`, allocated data,
BSS and inter-section padding remain separate. Unsupported words are reported
together; they are never silently treated as data. Sectionless or ambiguous
images need code/data classification before this mode can be used.

Mixed copied payload mappings may append `code=offset:size` extents after their
entry offsets. The complete payload is retained, including its data; only the
declared code extents are compiled. These extents describe input layout, not
runtime dispatch exceptions. `code-regions.tsv` and `code-coverage.txt` record
the selection. Unknown runtime pointer values do not prevent selecting all
declared code, but a successful inventory does not prove every value is valid
or establish coverage of undeclared/loaded executable content.

Verification reports gaps before LLVM generation or a native build. Exit code 2
means unresolved flow; zero only establishes the checks performed over discovered
code. See [flow verification](flow-verification.md) for options and report columns,
and [batch conversion](batch-conversion.md) for module inventory and build commands.

## Discovery and runtime pointers

Roots come from entry points, validated executable metadata, direct control flow
and supported callback analysis. Bounded data-flow analysis tracks pointer copies,
stack aliases, stores, returns, tables and helper relationships. Unknown effects,
overlapping writes and ambiguous paths must invalidate unsafe conclusions.

A pointer-shaped word is not automatically code. Executable sections may contain
VU programs, tables and data. Undiscovered executable content and unresolved
references remain in coverage reports rather than becoming fabricated entry points.

Static candidate discovery does not make writable pointers constant. Generated
code must read the runtime target and either call its compiled binding or report
an unsupported target. New observations can guide a later offline conversion;
they never authorize runtime code translation.

## Generated execution

LLVM units are bounded and retain branches with their delay slots. Native adapters
preserve guest registers, continuation state and service boundaries. Existing
original-address dispatch and CPU-state interfaces are transitional: the intended
ABI uses recovered functions, native symbols and typed callback bindings.

Preserve integer widths, signedness, packed lanes, shift behavior and delay-slot
state without host-language undefined behavior. Floating-point and VU operations
must retain supported rounding, range and lane semantics; unsupported modes stop.
Operation budgets support diagnostic resumption and are not PS2 timing models.

Memory layout conversion distinguishes initialized data, BSS, copied payloads and
compiled instruction ranges. Native aliases share the intended backing storage.
Writable data inside an executable segment is not automatically immutable, while
writes that alter compiled code must be diagnosed. Unknown copied code requires
an explicit offline mapping and conversion, not an interpreter.

Scratchpad submissions use synchronous, validated native copy batches with
16 KiB wrapping. Normal copies work in both directions; supported source chains
can include full descriptor transfers, and destination chains use explicit RAM
write ranges. Completion is published after all copies commit. Interleave,
scratchpad self-copy, VU-storage transfers and unsupported descriptor kinds stop
explicitly. Native RAM destinations remain subject to compiled-code protection.

## Services and output evidence

Native services provide supported thread, callback, synchronization, file, audio,
input and graphics behavior. Initialization and pending/completed states must reflect
real ownership and operations. No success stubs may hide missing services.

For an explicitly assembled native environment, link a converted IOP bundle with
`-DANYPS2_IOP_PROJECT=<generated-project>` and launch with
`--iop-default-session <module.data>`. Empty-argument resets restart that bundle's
compiled initializers and boot callbacks through the existing lifecycle. The
bundle must provide its own completion publication; returning from an initializer
alone does not establish readiness. This is explicit integration configuration,
not automatic discovery of a complete default environment. Named reboot archives
remain separately validated through `--iop-reset-session <module.data> --disc <iso>`.
Firmware-internal inspection and patch requests without a native implementation
stop explicitly; no synthetic firmware is exposed to satisfy them.

For ISO mounts, `--disc-media cd|dvd` supplies the optical-media identity used by
disc-type queries. It is independent of the program's selected read mode; an
unconfigured identity remains an explicit unsupported query rather than a size-based guess.

Generated projects retain LLVM, build configuration and diagnostics. Their dependency
audit checks for forbidden runtime decoders/interpreters/JIT dependencies. Validate
execution as well as generation and linking. Keep captures, original program data
and converted commercial code ignored.

GT4 has rendered its opening warning; title-screen and gameplay compatibility are
not established. Homebrew results and adaptation limits are summarized in
[project history](project-history.md). Current regression commands are in
[testing](testing.md); historical per-operation logs belong in Git history or local
investigation output, not this guide.
