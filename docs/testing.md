# Testing AnyPS2

## Default tests

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Default suites cover synthetic ISO/ELF parsing, control-flow discovery, compiled
generated instructions, kernel behavior, hardware registers, process arguments,
repository content, and rejection fixtures for the repository policy. They need
no commercial game or BIOS. A generated native fixture is compiled by the normal
build so tests execute the generator's output.

The content policy checks current source candidates and actual Git index blobs.
Ignored local artifacts are not candidates, but force-added artifacts are checked.
It allows only approved source/documentation paths and rejects recognizable
binary content, generated guest-code headers, oversized files, and output names.
New legitimate source locations require an explicit policy update and review.
This is not a copyright detector: human provenance review remains required.

Run the policy again after staging and review `git diff --cached` before any
commit. Nothing about a private repository permits proprietary game content.

## Opt-in local startup regression

Generate and build a local diagnostic executable as described in README.md.
Configure its paths and an expected diagnostic boundary explicitly:

```powershell
cmake -S . -B build `
  '-DANYPS2_TEST_PROBE=C:/local/output/build/Release/ps2-native.exe' `
  '-DANYPS2_TEST_IMAGE=C:/local/output/boot.elf' `
  '-DANYPS2_EXPECTED_STOP=stop=syscall pc=0xADDRESS'
ctest --test-dir build -C Release -R local-startup --output-on-failure
```

Replace the example paths and address with your local output and verified
expected boundary. The default instruction budget is one million; configure
`ANYPS2_TEST_BUDGET` for longer bounded runs. The test requires
diagnostic exit code 3, checks the supplied regular expression, and stores output
in ignored `build/local-startup.txt`. It fails on setup errors, budget exhaustion,
timeouts, crashes, or a changed boundary. Disable it with
`cmake -S . -B build -DANYPS2_TEST_PROBE=`. Never commit local captures or logs.

Probe exit codes are 1 for setup/runtime errors, 3 for diagnostic stops, and 4
for budget exhaustion. A passing startup regression is not proof of game boot.

## Discovering additional code

The recompiler accepts repeated `--entry=0xADDRESS` arguments for observed code
entries. An optional `--capture=DIRECTORY` supplies a matching local RAM capture
for code copied after initial load. Captures contain `ram.bin`, `image-id.txt`,
and `observed-entries.txt`. They are analysis evidence, not a full saved machine
state. The probe's `--capture=NEW_DIRECTORY` option can collect these artifacts
when explicitly requested by a local test harness. Keep all such directories
under ignored `work/`.

Do not turn observed game instruction words into committed regression fixtures.
Construct a small original synthetic program reproducing the relevant behavior.

## Latest local GT4 boundary

The BIOS-free startup test reached 3,803,081 retired instructions and 71 serviced
kernel calls, stopping with a pending DMAC interrupt at PC 0x0102251c.
The test used a 30-million-instruction upper bound. This passed the former
SIF0 DMA-register boundary and processed two IOP command packets. The IOP endpoint
returned the RPC-initialization software-register reply over SIF0. The recorded
CPU pending line is 0x800 and D_STAT is 0x00200060: SIF0 completion is enabled and
awaiting delivery to the registered guest handler.

Synthetic generated-code tests separately verify thread switching and restarting,
full register preservation, priority changes, equal-priority ordering, and
syscalls in delay slots. Arithmetic tests cover signed division overflow and
zero divisors, unsigned result sign extension, PLZCW lanes, and every byte offset
for LDL/LDR/SDL/SDR. SIF tests cover payload movement, tag chains, buffering,
handshakes, and malformed inputs. There is no verified visible game boot.
Interrupt handler delivery, actual RPC services, and full IOP execution remain
unfinished; no game-specific patch is used to skip them.
