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
   continuations. The guest table is authoritative. Interrupt registration exists;
   delivery remains unfinished. A separate thread module manages dormant/ready
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

- Compiler correctness: generated synthetic programs pass semantic tests.
- Startup progress: an opt-in integration test reaches a documented boundary.
- Boot: verified display and input through the game's startup sequence.
- Playability: sustained rendering, input, sound, loading, and gameplay.

Only the first two have implementation evidence so far. An expected diagnostic
stop is a passing regression test, not a successful game boot.
