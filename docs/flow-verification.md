# EE flow verification

Run verification before LLVM emission and native compilation to collect discovery
gaps in one report. It uses the same EE discovery closure, payload mappings and
secondary-image preparation as conversion. It does not run the game or introduce
a runtime interpreter.

```powershell
.\build\Release\ps2-recompile.exe work\game\boot.elf work\flow-report --verify-only
```

For a conversion with copied payloads and a secondary executable image, include
the same inputs:

```powershell
.\build\Release\ps2-recompile.exe work\game\boot.elf work\flow-report --verify-only `
    --payloads work\game\payloads.txt --image work\game\secondary.elf
```

The output directory must be new. Reports contain private input addresses and
must remain ignored alongside other commercial-game investigation artifacts.
No LLVM files, compiled objects, game-data image or build project are emitted.

## Results

| Exit code | Meaning |
| --- | --- |
| `0` | The reached-flow inventory has no reported unresolved transfers or unclassified references, and its reached instructions and layout pass conversion checks. |
| `2` | Verification completed and found gaps. Read the reports before choosing the next implementation batch. |
| `1` | Input, layout preparation, option or output error; no completed report is published. |

An exit code of `0` is not a complete-game compatibility certificate. Unknown
dynamic content can remain outside discovered flow. Native service implementation,
compiled execution, rendering and gameplay require separate validation. This pass
checks EE images; the existing conversion inventory and IOP/VU tools cover those
separate conversion inputs.

## Reports

| File | Evidence |
| --- | --- |
| `flow-verification.txt` | Counts, incomplete/closed reached-flow status and explicit generation/compilation/execution status. |
| `discovery-passes.tsv` | Per-image iterations, instruction counts, new roots and new registered services. The final pass adds neither roots nor registrations. |
| `flow-gaps.tsv` | Every unresolved indirect transfer, unmapped target and unclassified pointer reference identified by the existing analysis. |
| `callback-flow-gaps.tsv` | Stored pointer candidates whose field matches an observed function/context consumer, with source, target, field and consumer sites. Object identity remains unproved. |
| `call-sites.tsv` | Reached direct and indirect calls, returns and native service boundaries. Verification uses `discovered` labels, never `compiled`. |
| `pointer-references.tsv` | All identified pointer references, including callable evidence, data uses and unresolved classification. |
| `lowering-diagnostics.txt` | Unsupported reached instruction families and lowering constraints. |
| `layout-diagnostics.txt` | Native address-layout validation errors. |
| `input-identity.txt` | Path-independent identity of the ordered ELF inputs and copied-payload mappings. |
| `observed-call-edges.tsv` | Optional validated native-stop edges used by this conversion, separate from static discovery evidence. |

Discovery already repeats until its implemented rules find no additional roots.
That fixed point does not prove the rules are complete. A missing relationship
between an initializer and consumer can leave a real callback unclassified even
though its address was identified at a store.

Use the callback report to investigate related source-to-consumer gaps together.
A matching field offset and record shape are only leads: prove the object
relationship, preserve overwrite and alias rules, and add positive and negative
synthetic fixtures before extending discovery. The verifier never adds these
candidates as compilation roots. Do not use the report as a list of addresses to
compile blindly or bypass an unknown runtime target.

After a coherent discovery repair, rerun verification, compare the gaps, generate
and compile the actual project, then validate GT4 execution. Fewer report rows,
successful lowering or more discovered instructions alone do not establish boot.

## Offline native-call observations

When a native run stops at an undiscovered indirect destination, retain its
`--capture-stop` directory. A subsequent conversion can consume that observation:

```powershell
.\build\Release\ps2-recompile.exe work\game\boot.elf work\next-project `
    --observation work\native-run\stop
```

Supply the same payload mappings and secondary images as the original build.
Repeat `--observation` for multiple captures (at most 64); input order does not
matter. `--verify-only` also accepts these inputs. After conversion, compile the
new LLVM artifacts through the normal native build. The running executable never
decodes, compiles or substitutes an unknown instruction, and it still stops on
destinations absent from its compiled code. The observation is a conversion input,
not a resumable save state or a runtime dependency.

Only complete v2 captures emitted by a generated project with an input identity
are accepted. Old captures require a rebuilt executable and a fresh run. Checks
cover matching input identity, active image, a completed physical-RAM transfer,
an already reached indirect source and its delay slot, the call's return link,
and a file-backed EE target. Data, BSS and declared VU sections are excluded. All
reached instruction bytes in the observed image must match the original inputs;
modified or dynamically generated code is not silently extracted from RAM.
Sources reached through an earlier accepted observation are resolved by the same
bounded discovery process. Disconnected observations cannot create their own
starting point. Reached unsupported operations retain ordinary conversion errors.

Input identity uses the existing non-cryptographic build fingerprint and catches
stale inputs; it does not authenticate the producer or prove a capture was not
edited. Use your own unmodified native captures. Captures and derived reports can
contain proprietary material and remain private under ignored `work/`.

A witnessed destination proves one observed transfer, not the full target set of
a mutable callback. `call-sites.tsv` therefore retains unresolved-indirect rows
even after a witnessed target is compiled. Cross-image calls, address aliases and
runtime-generated code outside this observation contract require separate
analysis; acceptance does not imply general dynamic-code coverage. The native
synthetic test executes two nested computed callbacks across offline rebuilds,
including target-register writes in delay slots, and rejects stale, malformed,
non-executable and modified-code observations.
