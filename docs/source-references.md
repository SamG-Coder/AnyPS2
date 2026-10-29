# Source references and provenance

AnyPS2 uses public instruction/ABI interfaces and local open-source
references. No BIOS image or BIOS disassembly was used for this implementation.

- PCSX2, revision `1f67c53b6723fcd6a90041a5b296f0e34a6a3222`:
  https://github.com/PCSX2/pcsx2
  `R5900OpcodeTables.cpp`, `R5900OpcodeImpl.cpp`, `MMI.cpp`, `FPU.cpp`,
  `COP0.cpp`, and timer register implementation
  were consulted for EE operation behavior and classification. PCSX2 source is
  currently unmodified and not linked. Future component reuse must retain its
  applicable licenses/notices.
  `Sif0.cpp`, `Sif1.cpp`, `Sif.h`, `Dmac.cpp`, `HwRead.cpp`, `HwWrite.cpp`,
  `Hw.cpp`, and `Cache.cpp` were also consulted for FIFO capacity, tag behavior,
  DMA enable/reset values, register directionality, and data-cache operations.
- PS2SDK, revision `2a18d15da1332665451a9bcf9a0d4df9029c9490`:
  https://github.com/ps2dev/ps2sdk/blob/master/ee/startup/src/crt0.c
  https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h
  https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/syscallnr.h
  These establish syscall numbers, startup argument registers, semaphore fields,
  and the thread descriptor/status layouts. `ee/kernel/include/osd_config.h`
  defines the user configuration bit fields. No PS2SDK library is linked or
  required at runtime.
  `ee/kernel/include/sifdma.h`, `common/include/sifcmd-common.h`,
  `common/include/sifrpc-common.h`, and the public EE/IOP `sifcmd.c` implementations
  establish descriptor layouts, command IDs, initialization messages, and RPC
  reply correlation fields. The C++ endpoint is an independent HLE implementation;
  it does not embed those libraries or any Sony firmware.
- Play!, revision `1f27232772e484bfb79364b751b84f41f4c87481`:
  https://github.com/jpd002/Play-/blob/master/Source/ee/PS2OS.cpp
  Startup stack reservation (0x2a0), default top-of-RAM handling, heap limit,
  semaphore return conventions, guest syscall table/callback behavior, and the
  timer-3 registration alias informed the HLE implementation. Thread creation,
  dormant/ready states, priority-zero support, startup register conventions,
  deletion, and status return behavior also informed the thread services.
  DMAC callback argument/GP conventions, channel and chain ordering, negative
  callback return handling, and default uncached user RAM ranges informed the
  independent C++ interrupt dispatcher and shared RAM address translation.
  PS2SDK's public `UNCACHED_SEG`/`UCAB_SEG` macros and `ExitHandler` interface
  were also consulted. No BIOS instructions are generated for the HLE return gate.
  The same public reference's INTC dispatcher informed controller priority,
  ascending INTC line order, acknowledgement and callback ABI. Its SetSyscall
  timer-3 alias uses zero argument and GP. INTC reuses the C++ DMAC context/return
  mechanism; no proprietary interrupt-vector code is embedded.
  PS2SDK `iop/fs/fileio/src/fileio.c` and Play! `Source/iop/Iop_FileIo.h`
  identify RPC server 0x80000001 as file I/O. The public `fileio-common.h` and
  `sifrpc-common.h` layouts, plus Play! `Iop_FileIo.cpp` and
  `Iop_FileIoHandler1000.cpp`, informed the independent legacy read-only service:
  method IDs, version negotiation, arguments, results, and completion correlation.
  The implementation reuses AnyPS2's ISO reader and contains no proprietary module.
  The public `Config2Param` structure and Get/SetOsdConfigParam2 declarations
  define the four volatile settings bytes supported by the HLE kernel.
  PCSX2's LWL/LWR/SWL/SWR semantics informed the shared word/doubleword merge
  helpers, including the EE's partial-LWR upper-word preservation rule.
  Public MMI lane definitions informed packed equality, even-lane packing, and
  five-bit color packing/expansion. Synthetic vectors validate both 64-bit halves
  and aliased operands without including game instructions.
  The public EE scratchpad mapping supplies an independent 16 KiB RAM region;
  HLE SIF submissions read it through the same bus rather than a game-specific copy.
  This is compatibility evidence, not proof of all hardware/kernel edge cases.
  Its attribution/license is retained in [Play-license.txt](third-party/Play-license.txt).

PCSX2 `pcsx2/GS.h` and `pcsx2/GS.cpp` at the pinned revision above were
consulted for privileged register addresses, CSR acknowledgement/reset, empty
FIFO status, revision/ID defaults, and IMR writable/fixed bits. AnyPS2's separate
C++ register model implements these interfaces without firmware or renderer code.
PS2SDK's public kernel declarations/syscall numbers and Play!'s GsGetIMR and
GsPutIMR services informed the HLE forwarding to the same MMIO register model.
Play!'s `GSHandler::SetCrt` supplies the public mode/SMODE2 relationship.
PCSX2 `Counters.cpp` supplies interlaced NTSC/PAL field rates, blanking lengths,
delayed GS field events and separate INTC blanking edges. The independent integer
scanout model is diagnostic timing, not a copy of BIOS CRT programming tables.
PCSX2 `Vif.cpp`, `Vif.h` and `Hw.h`, together with Play! `ee/Vif.cpp` and
`ee/Vif.h`, informed the independent VIF register model: register addresses,
idle stop/break/cancel flags, reset commands, row/column preservation, and VIF1
reverse-transfer control. Command execution and active DMA are not inferred
from idle-register behavior.
PCSX2 `VU0.cpp`, `VU0micro.cpp` and `VU1micro.cpp` define the CFC2/CTC2
FBRST control masks and per-unit reset clearing used by the initialization-only
VU control interface. No vector instruction or microprogram implementation is copied.

The local reference checkouts under `third_party` are ignored. Game images,
extracted executables, generated guest code, and runtime traces stay under the
ignored `work` directory. The generic implementation and tests contain no GT4
instruction blobs or address-dependent game patches; the tests use synthetic
instruction fixtures.
