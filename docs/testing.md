# Testing AnyPS2

## Default tests

Add tests for major behavior changes and integration milestones. Extend an
existing subsystem regression when practical; small edits and helpers do not
need separate tests that duplicate their implementation.

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
The disc-file-I/O milestone uses a synthetic ISO and exercises the actual SIF
request/reply path. No commercial image is needed for that test.

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
diagnostic exit code 3 by default, checks the supplied regular expression, and stores output
in ignored `build/local-startup.txt`. It fails on setup errors, budget exhaustion,
timeouts, crashes, or a changed boundary. Disable it with
`cmake -S . -B build -DANYPS2_TEST_PROBE=`. Never commit local captures or logs.

Probe exit codes are 1 for setup/runtime errors, 3 for diagnostic stops, and 4
for budget exhaustion. A passing startup regression is not proof of game boot.
Set `ANYPS2_TEST_DISC` to the user's ISO to enable the read-only FILEIO server.
It is passed as a separate `--disc=...` argument, including paths with spaces.
Leave it empty for tests of the unmounted endpoint.
For an intentional bounded-execution regression, explicitly configure
`ANYPS2_EXPECTED_EXIT=4` and an expected expression covering the budget stop and
relevant progress counters. This records a repeatable running/waiting boundary;
it does not treat budget exhaustion as successful boot. Other exit codes are
rejected. Restore `ANYPS2_EXPECTED_EXIT=3` for diagnostic-stop regressions.

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

With the user's ISO mounted, the BIOS-free startup test passes CSR initialization
and GsPutIMR, completes SetGsCrt for interlaced NTSC output, then stops at the
unsupported VIF0 FBRST register write (address 0x10003810). It retires 5,968,896
instructions and services 108 kernel calls. No scanout edge is reached by this
short interval after mode setup; scanout behavior is verified by synthetic tests.
Three interrupt dispatches completed and five IOP packets were consumed.

The guest binds to FILEIO on its first attempt and completes its version query.
It then passes additional startup functions, unaligned word operations, packed
string operations, OSD2 byte reads and scratchpad accesses. At the VIF stop it has
issued one FILEIO request (version), zero successful opens, and zero bytes read.
Actual file reads are verified by the synthetic disc/RPC integration test;
the game has not yet exercised them. Fewer instructions than the previous
300-million-instruction wait reflect removal of the FILEIO bind retry loop.
The local regression again expects diagnostic exit 3, with a ten-million-step
budget and checks for the VIF stop, instruction count and FILEIO counters.

The existing hardware-register integration suite covers GS reset and identity,
display register lane writes and aliases, IMR access through MMIO and HLE,
empty-FIFO flush/acknowledgement, and rejection of unsupported transfers and
accesses without changing device state. Scanout coverage extends this milestone
with NTSC blanking edges, delayed GS field events, masking/unmasking, acknowledgement,
alternating fields, PAL phase preservation across fragmented advancement, invalid
mode rejection and reset cancellation. Before configuration no video events occur.
The generated-code suite delivers GS, VBlank-start and VBlank-end interrupts to
registered synthetic guest handlers and checks return/context restoration.

Synthetic generated-code tests separately verify thread switching and restarting,
full register preservation, priority changes, equal-priority ordering, and
syscalls in delay slots. Arithmetic tests cover signed division overflow and
zero divisors, unsigned result sign extension, PLZCW lanes, and every byte offset
for word and doubleword partial loads/stores. SIF tests cover payload movement, tag chains, buffering,
handshakes, and malformed inputs. There is no verified visible game boot.
Accurate scanout timing, VIF/GIF rendering, remaining RPC services, and full IOP execution remain
unfinished; no game-specific patch is used to skip them.
