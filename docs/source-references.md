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
  This is compatibility evidence, not proof of all hardware/kernel edge cases.
  Its attribution/license is retained in [Play-license.txt](third-party/Play-license.txt).

The local reference checkouts under `third_party` are ignored. Game images,
extracted executables, generated guest code, and runtime traces stay under the
ignored `work` directory. The generic implementation and tests contain no GT4
instruction blobs or address-dependent game patches; the tests use synthetic
instruction fixtures.
