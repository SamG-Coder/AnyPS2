# Architecture

## Contract and layers

AnyPS2 translates supported guest instructions into C++ and compiles them for the
host. Generated code calls shared runtime interfaces for guest state, memory,
devices, and BIOS-free services. Hardware compatibility remains necessary even
when CPU instructions have been translated ahead of time.

1. Binary/disc/ELF libraries validate sizes and ranges before loading. Import
   accepts ISO9660 with 2048-byte sectors and little-endian MIPS ELF32 ET_EXEC.
   Raw BIN, CHD, interleaving, and multi-extent files are unsupported.
2. The decoder and analysis worklist discover direct control flow, including
   delay slots. Unproven indirect destinations stop. Observed roots and
   image-matched RAM captures provide evidence for copied code.
3. The generator emits explicit guest-width C++ operations. Bounded non-inlined
   dispatch functions control function size and stack use. A shared loop handles
   PCs, delay state, budgets, code guards, and runtime stops.
4. The runtime owns register state and RAM. The system bus adds partial timer,
   INTC, DMAC, and partial SIF DMA transport. A separate BIOS-free IOP command
   endpoint owns IOP RAM and protocol state. Unsupported MMIO faults. Timer counting
   uses instruction-based diagnostic time, not accurate PS2 cycles.
5. The BIOS-free kernel implements startup services and a guest-visible syscall
   table. Guest handlers execute generated code through bounded nested
   continuations. The guest table is authoritative. INTC and DMAC delivery share
   a context-preserving callback dispatcher. A separate thread module manages dormant/ready
   records and priority-based context switching after supported kernel services.
   It saves integer and FPU state, preserves the CPU's global budget and CP0
   control state, and resumes the correct syscall/delay-slot continuation.
   Equal-priority readiness uses insertion order; a running thread is not
   preempted by an equal-priority start. Guest kernel continuations cannot switch
   threads. FIFO semaphore waits can block when another thread is runnable;
   signals wake the oldest waiter. Idle waits and timer-driven preemption remain
   unsupported. See [SIF transport](sif.md) for device and protocol boundaries.
6. Small programs parse arguments and invoke libraries. The build tool passes
   explicit arguments to processes without constructing shell commands.
7. The FILEIO service owns read-only file handles and uses bounded ISO range
   reads. SIF packet parsing and DMA reply transport stay in the hardware layer.
   Guest paths resolve only within the explicitly mounted image. See
   [file-I/O service](fileio.md) for protocol scope and limits.

GT4 is the first integration target. Game identifiers and observed addresses must
not select special behavior in generic libraries. Additional code entry points
are analysis inputs, not game-specific runtime patches.

## Copied and modified code

A capture contains RAM and an image fingerprint. The fingerprint detects
accidental mismatches; it is not cryptographic authentication. Capture discovery
starts at supplied observed entries. Conflicting instruction versions at one
address are rejected. Generated programs start from the original ELF and replay
its copies, rather than restoring a captured machine state. Instruction guards
prevent silently executing stale translations.

Commercial captures, generated C++, and executables are local-only artifacts.
Synthetic tests exercise the same mechanisms without commercial data.

## Performance direction

Current per-instruction dispatch and guards favor diagnosability. Future work
should form basic blocks, use direct known flow, share slow paths, hoist guards
only with valid invalidation, and batch execution to event deadlines. Keep
functions bounded and avoid duplicated runtime logic. Benchmark Release builds
with reproducible workloads after correctness tests. AOT alone does not establish
performance.

## Acceptance boundaries

VIF0/VIF1 share an initialization register model with independent state. Cold
state is idle with an empty FIFO; soft reset clears status and error masks while
preserving configuration and row/column data. FBRST stop, force-break and cancel
operate on idle units, and command bits are not retained. Widths, alignment,
register holes, reverse transfers and unsupported command-managed writes are
checked before mutation. FIFO payloads and active DMA remain unsupported.

CFC2/CTC2 generation forwards VU control accesses to a separate device interface.
FBRST supports the two units' control masks and reset strobes; VPU_STAT reports
idle because every execution-start path is still unsupported. Interlocked forms
are valid only under this idle-unit limitation. Other control registers and VU
microprogram execution stop explicitly. These interfaces require no BIOS code.

The hardware library owns a separate GS privileged-register model. Display
configuration is stored without producing pixels. CSR exposes
an empty FIFO and reference-compatible identity, accepts acknowledgements and
reset, and does not latch write-only commands. IMR preserves its fixed bits.
GsGetIMR/GsPutIMR forward through the bus, including interrupt-safe aliases.
Unknown registers, quadword accesses, local-to-host and external feedback
transfers stop explicitly.

SetGsCrt configures interlaced NTSC (mode 2) or PAL (mode 3), with either
field/frame framebuffer mode reflected in SMODE2. Unsupported modes fail before
changing state. This is host configuration, not a reconstructed BIOS register table.
An integer half-line clock uses a 294.912 MHz EE time base, 60000/1001 NTSC
fields per second or 50 PAL fields per second, with fractional cycles retained.
NTSC/PAL fields contain 525/625 half-lines. VBlank starts at half-line 480/576,
GS VSINT and FIELD update at 487/582, and VBlank ends at the field boundary.
INTC lines 2/3 receive blanking edges independently of GS IMR; GS events latch
CSR and raise INTC line 0 when unmasked. Unmasking a pending event also raises
the line; acknowledging INTC alone does not repeatedly retrigger a latched GS event.
Horizontal sync is approximated at whole-line boundaries, not pixel-clock phase.

Timing advances only after SetGsCrt and stops on CSR RESET. The runtime charges
one diagnostic cycle per retired instruction, so these nominal video intervals
are not cycle-accurate PS2 timing or real-time pacing. Direct timing reprogramming,
progressive modes, gated/HBlank timers, GIF rendering and display output remain
unsupported. Active unsupported timing changes fault instead of silently retaining
a mismatched clock configuration.

- Compiler correctness: generated synthetic programs pass semantic tests.
- Startup progress: an opt-in integration test reaches a documented boundary.
- Boot: verified display and input through the game's startup sequence.
- Playability: sustained rendering, input, sound, loading, and gameplay.

Only the first two have implementation evidence so far. An expected diagnostic
stop is a passing regression test, not a successful game boot.
