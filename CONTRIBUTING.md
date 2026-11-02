# Contributing to AnyPS2

Read [AGENTS.md](AGENTS.md), the [disclaimer](DISCLAIMER.md) and
[licensing guidance](docs/licensing.md) before contributing. Contributions to
original project code are submitted under its MIT license; retain any separate
third-party notices and identify the origin and license of adapted material.

Use C++20 for reusable services and LLVM AOT for converted code. Keep behavior
generic: no game-address hacks, successful-response stubs, runtime instruction
interpreters, JIT or emulator fallback. Preserve explicit unsupported diagnostics.

Provide an original synthetic reproducer for a confirmed major bug, run the
related Release tests, and report exactly what was verified. See
[testing](docs/testing.md) and [original PS2 programs](docs/ps2-test-programs.md).
AI-generated contributions receive the same review, provenance and testing requirements.

Never attach or commit BIOS/firmware, keys, proprietary SDKs, game images,
extracted modules, assets, saves, memory dumps, game instruction traces, translated
commercial IR/code or converted game binaries. Reproduce problems with synthetic
inputs where possible. Keep local investigation under ignored `work/` and reference
checkouts under ignored `third_party/`. Game ownership does not authorize redistribution.

Before staging and again after staging, run:

```powershell
ctest --test-dir build -C Release -R '^repository-content$' --output-on-failure
```

Review staged filenames and contents yourself; the guard cannot establish ownership.
Do not force-add ignored material. Report compiler/runtime versions, the failing
subsystem and synthetic reproduction steps in issues, rather than uploading private
captures. Link licensed public references and record pins in
[docs/source-references.md](docs/source-references.md).
