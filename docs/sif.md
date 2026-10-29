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
  RPC initialization returns a SET_SREG reply over SIF0. A mounted disc enables
  the read-only FILEIO server; other binds receive correlated null-server responses.
  Unsupported RPC calls, reset commands, and unknown commands stop explicitly.
  See [file-I/O service](fileio.md) for supported calls and queued DMA replies.

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
The HLE kernel delivers enabled DMAC completions to registered guest callbacks,
passing the channel, argument, and registration-time GP. It acknowledges selected
completion flags without toggling masks, visits higher channels first, and follows
the registered chain order. A negative callback result skips the remaining
callbacks on that channel. An HLE return gate restores the full interrupted CPU
context while retaining the global instruction count. Callback execution can
pause and resume at an instruction budget boundary. Thread scheduling is deferred
until callback completion.

INTC uses the same context and return path. Enabled INTC lines dispatch in
ascending order, while DMAC takes priority when both CPU interrupt lines are
enabled and pending. INTC acknowledgement preserves masked sources and the mask
register. AddIntcHandler callbacks receive their line, argument and saved GP;
the SetSyscall timer-3 alias receives zero argument/GP. Callback entry sets EXL
and clears IE; return restores the interrupted status.

Nested interrupts and modification of an active callback chain
remain unsupported. Pending channels without registered handlers remain pending.
Callback stacks currently require aligned physical RAM addresses. Default
uncached user RAM mappings cover 0x20100000Ã¢â‚¬â€œ0x21ffffff and
0x30100000Ã¢â‚¬â€œ0x31ffffff; this is a BIOS-free default mapping, not a general TLB.
RAM accesses and HLE SIF source descriptors share the same address translation.

FIFO semaphore waits
can now switch to another ready thread and wake on a signal; waits requiring an
idle scheduler, non-FIFO policies, and deleting a semaphore with waiters stop.

## Verification

SifStopDma clears the EE SIF0 start bit without changing the other direction,
queued IOP data, remaining count or completion flags. Restarting an untouched
paused channel retains the destination tag's end state; explicit channel register
setup starts a new transfer. The transport milestone pauses a partial payload,
verifies SIF1 still transfers, and resumes SIF0 through its genuine completion.
IOP reset command 0x80000003 supports mode zero with an empty argument or the
default `rom0:UDNL` service request. It waits for the incoming DMA job to finish,
requires SIF0 stopped, clears queued replies and volatile IOP/service state,
closes FILEIO handles and resets CD/DVD initialization. The mounted ISO and
cumulative diagnostics survive. Command readiness and BOOTEND return after a
bounded diagnostic startup phase; this is not firmware timing. The integration
test reinitializes RPC, verifies stale handles fail, and reads the mounted disc
again. Requests using `rom0:UDNL` with a disc path read the replacement archive
from the existing mounted ISO, with a 16 MiB limit. The reusable ROMDIR parser
validates the directory, aligned file extents, unique names, termination and
extended-information records. It accepts disc module archives with an empty
RESET entry; it does not parse or execute firmware ROMs. A private reboot context
loads the archive's SYSMEM module and runs its public RAM-size entry interface
with a bounded diagnostic instruction budget. Its RAM and execution state are
retained for subsequent bootstrap work. After the memory entry returns, its
export table supplies the guest allocator used to reserve the module-manager
region. The manager is relocated, bound to the initialized memory service, and
installed only after validation. Linked module records and the public boot
parameters point to a guest-allocated archive and terminated module list. The
manager entry then executes with that handoff in bounded slices alongside EE
execution. The reset packet completes transport before those slices begin.
EE-owned SBUS registers survive the IOP reset; IOP readiness and prior HLE
services are cleared. EE SIF driver initialization publishes its own transport
flag independently of IOP readiness. Budget yields preserve CPU state and let
the peer run. Unsupported accesses and returned entries stop with a retained
failure; service-readiness handling is still unfinished. None of these states
sets BOOTEND or claims that service reset has completed. HLE initialization
cannot replace the running disc context. Guest SIF1 destination-chain DMA now
consumes the shared FIFO into that context's RAM; it does not dispatch packets
through the obsolete HLE service buffer.

The mounted-disc endpoint also binds CD/DVD initialization server 0x80000592.
Method zero accepts the four-byte INIT, INoD or EXIT mode and returns a sixteen-byte
reply through the existing SIF0 completion path. INIT/INoD establish service state
against the already validated ISO; EXIT clears it. Version/debug fields are zero.
This does not implement sector reads, drive commands or an IOP firmware module.
The disc RPC milestone verifies bind, init/reinit/exit, response layout and
completion correlation alongside the existing FILEIO operations.

Synthetic tests cover both DMA directions, every supported source tag kind,
tag-transfer assembly, queue backpressure, enable/suspend, memory bounds,
completion acknowledgement, CPU interrupt masks, command handshakes, null RPC
binding, malformed messages, and HLE batch validation/status. Generated-code
tests cover coherent data-cache operations and semaphore context switching. One
DMA integration regression covers SIF0 payload delivery, generated guest callbacks,
budget suspension, chain termination, context restoration, and an outbound reply
through uncached RAM. Tests target major behavior changes and milestones.
The timer integration scenario covers regular and alias registrations, masking,
delay-slot deferral, budget resumption, concurrent DMAC/INTC priority, preserved
CPU context, and leaving unregistered interrupts pending.
No commercial instruction or packet payload is included in these tests.

During disc-backed reboot, guest SIF0 source-chain DMA now sends descriptors and
payload from private IOP RAM into the existing EE FIFO. Guest replies therefore
reach the ordinary EE destination-chain receiver and interrupt path. This does
not synthesize RPC replies or establish that all disc services are ready.
