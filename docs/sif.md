# SIF transport and BIOS-free IOP endpoint

## Responsibilities

`nt_hardware` separates the SIF transport (`sif.cpp`), bounded HLE submission
queue (`sif_jobs.cpp`), and IOP command dispatcher (`sif_commands.cpp`).
`nt_kernel` translates SIF syscalls into the runtime bus interface. Both direct
EE DMA register access and HLE submissions use the same FIFOs and IOP receiver.

The endpoint owns 2 MiB of IOP RAM. It is not an R3000 CPU implementation and
loads no BIOS or firmware. Its command buffer and reply scratch space are HLE
allocations, not addresses copied from a retail BIOS.

## Implemented behavior

- SIF0/SIF1 CHCR, MADR and QWC; SIF1 TADR; DMAC global enable/suspend and completion
  status. INTC and DMAC use distinct CPU interrupt-mask bits.
- Bounded 128-word FIFOs, producer backpressure, payload movement, address/count
  updates, and completion after transfer. Empty SIF0 destination chains wait.
- SIF0 normal transfer and CNT/END destination tags; SIF1 normal transfer and
  REFE/CNT/NEXT/REF/END source tags, tag-transfer data, and IRQ tag termination.
- EE/IOP command-address and flag registers with directional set/clear behavior.
- SifSetDChain, SifSetReg, SifGetReg, SifSetDma and SifDmaStat. HLE batches contain
  at most 32 descriptors, validate completely before queuing, and report pending
  until the IOP receiver has consumed their data. A busy transmitter returns zero.
- Command initialization, receive-address changes, and 32 software registers.
  RPC initialization returns a SET_SREG reply over SIF0. The RPC server registry
  is currently empty; bind requests receive correlated null-server responses.
  RPC calls, reset commands, and unknown commands stop explicitly.

SIF initialization publishes transport/command readiness only after installing
the corresponding HLE endpoint. It does not assert complete IOP boot or invent
game service results. The RPC initialization acknowledgment establishes the
minimal dispatcher, not the presence of file, audio, controller, or module services.

## Limits

Transfers currently advance one bounded step per modeled EE cycle. This is a
deterministic diagnostic model, not measured PS2 bus timing. The IOP receiver is
an HLE endpoint rather than independently scheduled IOP DMA hardware. Stall and
MFIFO modes, source-chain CALL/RET/REFS, destination CNTS, scratchpad DMA,
unsupported access widths, and active-channel reconfiguration are rejected.

HLE DMA supports normal/ERT/output-interrupt descriptor flags; other attributes
are rejected. It rounds byte counts to quadwords and rejects out-of-range memory.
Only one HLE batch owns the transmitter at a time. Software must not modify
submitted source data before completion. Completion acknowledges transport;
command processing can still fail and preserves its diagnostic reason.

The BIOS-free CPU handoff enables IE/EIE and the INTC/DMAC mask bits. Generated
code recognizes pending interrupts outside delay slots and exception level.
Delivery to registered guest handlers remains unfinished. FIFO semaphore waits
can now switch to another ready thread and wake on a signal; waits requiring an
idle scheduler, non-FIFO policies, and deleting a semaphore with waiters stop.

## Verification

Synthetic tests cover both DMA directions, every supported source tag kind,
tag-transfer assembly, queue backpressure, enable/suspend, memory bounds,
completion acknowledgement, CPU interrupt masks, command handshakes, null RPC
binding, malformed messages, and HLE batch validation/status. Generated-code
tests cover coherent data-cache operations and semaphore context switching.
No commercial instruction or packet payload is included in these tests.
