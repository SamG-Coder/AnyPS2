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
  SleepThread, WakeupThread and CancelWakeupThread establish sleeping state,
  queued wakeup consumption and cancellation returns. PS2SDK syscall numbers
  and its interrupt-wakeup notes inform the corresponding interrupt-safe aliases.
  The host idle loop advances devices with a bounded budget; it embeds no idle
  firmware code and does not count idle steps as retired guest instructions.
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
PCSX2 `Gif.h`, `Gif_Unit.h`, `Hw.h` and `HwWrite.cpp` informed GIF register
addresses, CTRL reset/pause, MODE path-3 masking/intermittent mode, and STAT
readback. The independent initialization model accepts no packets and does not
reset DMA or GS display configuration when GIF is reset.
PCSX2 `Dmac.cpp` and `Hw.h` supply the VIF0/VIF1/GIF DMA register layout,
16-bit QWC storage and CHCR start bit. Idle setup is shared across these three
channels; starting a transfer remains an explicit unsupported boundary.
PS2SDK `ee/dma/src/dma.c` also initializes the offset-0x80 scratchpad slots on
these channels to zero; that initialization is accepted, while nonzero setup stops.
`Dmac.h` defines the D_PCR CPC/CDE fields and PCE enable bit. Configuration
readback is supported with PCE disabled; priority-controlled arbitration stops.
PCSX2 `VUops.cpp`, `VUflags.cpp`, `VU0.cpp` and the public COP2 opcode tables
inform masked VSUB, constant VF0, raw vector transfers and flag layout. Its
AdvancedSettingsWidget documents the default VU chop/zero rounding mode. AnyPS2
uses independent integer arithmetic for finite subtraction, with explicit stops
for exponent-255 operands/overflow and functional rather than pipeline timing.
The same public VU arithmetic templates define VADD and x/y/z/w broadcast
selection. Addition and subtraction share one integer arithmetic implementation;
no per-game arithmetic overrides are used.
Play! `ee/COP_VU.cpp` supplies the VI0 constant-zero rule and 16-bit masking
for CTC2/CFC2 integer-register transfers, which remain separate from raw vectors.
PCSX2 `FPU.cpp` defines raw MOV.S/ABS.S/NEG.S bit operations and clearing of
current underflow/overflow flags by ABS.S/NEG.S while preserving sticky flags.
Its comparison helpers supply signed-zero/denormal equivalence, exponent-255
clamping for scalar comparisons, the FCR31 condition bit and BC1 likely annul.
`FPU.cpp` also informs scalar multiply sign/denormal handling and current/sticky
underflow flags. `AdvancedSettingsWidget.cpp` documents default EE chop rounding.
AnyPS2 computes finite products independently using 24-bit integer significands.
The multiplier precision caveat in `x86/iFPU.cpp` remains a limitation: this
implementation does not reproduce the PS2 partial-product truncation quirk or
apply the reference's game-specific workaround. Extended-range operands/results
stop before state changes.
PCSX2 `COP0.cpp` and the performance-register fields in `R5900.h` establish
MFPS/MTPS/MFPC/MTPC selector behavior, 32-bit counter storage and sign-extended
reads. Event 1 counts diagnostic cycles with CTE, ERL and privilege gating.
Event 6 reports no misses in the coherent-memory model, following the reference's
approximation; hardware cache behavior is not measured or emulated. Other active
events and overflow exceptions stop explicitly. The registers live outside
per-thread/interrupt register snapshots.
PCSX2 `Hw.cpp` source-chain transitions, `Vif0_Dma.cpp`/`Vif1_Dma.cpp` transport
and `Vif_Codes.cpp` command handlers inform the incremental VIF DMA receiver.
The same handlers define BASE/OFFSET/ITOP ten-bit fields, staged address updates,
and FLUSH waiting on VU and GIF paths 1/2. The current implementation waits on
pending path-2 payloads; it rejects VIF1-only commands on VIF0 rather than emulating the
unsupported error-interrupt path.
The independent implementation supports REFE/CNT/NEXT/REF/END, tag-data delivery,
scratchpad selection and normal completion. NOP/STCYCL/STMOD/MARK, STMASK,
STROW/STCOL, idle FLUSHE and MPG storage are implemented; no proprietary
microprogram is embedded and uploaded programs are not yet executed. Transfer
steps are diagnostic scheduling, not accurate DMA or VU timing.
PCSX2 `GS/GSRegs.h`, `GS/GSState.cpp` and `Gif_Unit.cpp` inform GIF tag fields,
PACKED A_D dispatch, REGLIST packing, GS register addresses and masked
SIGNAL/LABEL updates. The path-3 receiver uses the shared source-DMA transport.
`Vif_Codes.cpp` DIRECT supplies the immediate quadword count (zero means 65,536)
and path-2 destination. AnyPS2 uses an independent four-word assembler and reuses
its existing GIF decoder, retaining DMA input when paused or another packet owns
the receiver. Full GIF arbitration and DIRECTHL are not implemented.
`GSState.cpp` ApplyPRIM, XYZ/XYZF handlers and VertexKick inform primitive restart,
attribute layout and skipped-draw topology. AnyPS2 uses a bounded three-vertex
assembler. Tests use independently
constructed coordinates and attributes, not commercial command packets.
`GSRasterizer.cpp` sprite coverage and `GSRendererSW.cpp` framebuffer/depth write
selection inform ceil-based rectangle coverage and ZTE-disabled depth masking.
The independent solid-sprite implementation reuses AnyPS2's local-memory swizzle,
with explicit rejection of unsupported pixel-pipeline features.
`GSRegs.h` defines TEX0/TEX1/CLAMP/TEXA layouts; `GSDrawScanline.cpp` defines
MODULATE/DECAL and TCC channel selection. The Vulkan sampler independently
implements integer fixed-UV interpolation and direct-colour texture addressing.
The source texture snapshot is an explicit approximation of texture visibility.
`GSTables.cpp` PSMT4 swizzle layout and `GSClut.cpp` load conditions/CSM layouts
inform the indexed path. AnyPS2 independently expresses nibble addressing as bit
operations and stores loaded palette colours in its transactional GS state.
Tests use original palette colours and selected address-boundary cases.
`GSDrawScanline.cpp` bilinear sampling uses the half-texel offset and horizontal
then vertical four-bit interpolation after sample expansion. AnyPS2 implements
these operations in its shared Vulkan sampler; tests cover each weight pair.
Accepted configuration writes are synchronous; FINISH applies only to accepted
work. Vertex processing, other transfer directions and repeated SIGNAL
stall queues remain explicit boundaries. No renderer or proprietary packet data
is embedded in the implementation or synthetic fixtures.
PCSX2 `GS/GSTables.cpp` documents the PSMCT32 block/column address permutation,
also used by PSMCT24. AnyPS2 expresses this mapping with bit operations rather
than copying the tables. Public BITBLTBUF/TRXPOS/TRXREG fields in `GS/GSRegs.h`
define destination base, pitch, coordinates and extent. IMAGE uploads write
32/24-bit pixels into four MiB of local memory, preserving the unused high byte
in 24-bit mode. The implementation remains independent of commercial image data.
PCSX2 `FPU.cpp` supplies signed CVT.S.W operand semantics and unchanged flags.
`x86/iFPU.cpp` division helpers define clearing current I/D flags and setting
sticky flags for zero denominators. Its separate division rounding configuration
defaults to nearest. AnyPS2 uses independent integer significand operations for
chop-rounded conversion and nearest-even finite division, without host rounding
mode dependence. Extended-range division remains an explicit unsupported case.
`FPU.cpp` ADD_S/SUB_S informs scalar current/sticky U/O flag updates. Finite
scalar addition/subtraction reuse the independently implemented VU significand
core; numeric validation precedes any destination or flag update. This bounded
chop model does not establish complete EE extended-range or pipeline behavior.
The same `FPU.cpp` CVT_W implementation supplies float-to-integer truncation and
signed saturation thresholds, including exponent-255 inputs. AnyPS2 implements
these using integer bit operations without undefined host float-to-int casts.
`FPU.cpp` fp_min/fp_max and MIN_S/MAX_S establish raw signed-magnitude ordering
and clearing current U/O flags. AnyPS2 uses monotonic integer keys, retaining
the selected input bits rather than host IEEE NaN/denormal comparison semantics.
`R5900OpcodeTables.cpp` and `MMI.cpp` supply the six PEXT lower/upper byte,
halfword and word encodings and Rt-first lane ordering. AnyPS2 implements these
with one independent width-parameterized interleaving helper and synthetic tests.

The Vulkan backend is original C++/GLSL using the public Vulkan SDK interface,
tested with SDK 1.4.357.0 and a Vulkan 1.1 API baseline. It follows public
[synchronization rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
and [swapchain presentation interfaces](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html).
SDK headers, loader binaries and validation layers are external build/runtime
dependencies, not copied into the repository. Embedded SPIR-V is generated only
in build directories from the project's original shader source.

PS2SDK `kernel.h`, `syscallnr.h` and `iopcontrol.c` identify syscall 0x6b as
the no-argument, void SifStopDma operation on SIF0. The implementation stops
the EE receiver while preserving independent SIF1 work and buffered payload.
`sifcmd-common.h` and `iopcontrol.c` define reset packet bounds, argument length,
mode and readiness flags. AnyPS2 resets only its own HLE services for the default
request. Disc-supplied archives are parsed and the memory bootstrap executes
through the generic IRX loader and diagnostic CPU; subsequent module-manager
startup remains unsupported. No ROM contents or firmware startup routine is
reconstructed.

PS2SDK `tools/romimg/src/romimg.h` describes the public ROMDIR archive layout:
16-byte directory records, aligned file data, and bounded EXTINFO fields. The
independent C++ parser accepts empty-RESET disc archives and uses original
synthetic fixtures. No extracted module bytes or firmware implementation are
included in source or tests.

The IRX loader uses the public ELF/IRX layout emitted by PS2SDK
`tools/ps2-irxgen` and the relocation behavior documented by Play!
`Source/iop/IopBios.cpp` at the revisions above. It independently implements
bounded version-one IRX loading, BSS initialization and base-relative word,
jump and paired HI16/LO16 relocation in private storage. The existing Play!
notice is retained. No proprietary module instructions are used as fixtures.
PS2SDK `iop/kernel/include/irx.h` defines import-table names, versions, ordinal
stubs and terminators. Play!'s `IOPMOD` declaration documents module metadata.
The independent binder resolves through a caller-provided interface and patches
guest jumps only after validating all requests; it does not embed an upstream
kernel or treat unresolved imports as successful calls.
The export registry uses the same public `irx.h` layout and zero terminator;
Play! `Iop_Dynamic.cpp` was also consulted for ordinal-table structure. Registration
is explicit and accepts only validated guest function addresses owned by the
loaded image. Exact ABI matching is the current supported subset, not a claim
to implement the complete loadcore replacement/version policy.
The original diagnostic IOP integer core implements public MIPS-I encodings;
PCSX2 `R3000AInterpreter.cpp` was consulted for conditional link and branch
behavior. No upstream interpreter or firmware routine is copied or linked.
Execution tests contain independently constructed instruction sequences.
The startup-call ABI follows the public module entry interface and Play!
`IopBios.cpp` argument preparation: argc/argv, a terminated pointer table, GP,
and an argument save area on the stack. AnyPS2 independently constructs bounded
guest arguments and uses an out-of-RAM return sentinel rather than firmware
startup code. The caller retains responsibility for allocation and module state.
PS2SDK's public system-memory module entry declaration takes the RAM size as a
single integer rather than argc/argv. The generic register-argument call API
allows callers to select that interface without filename-specific runtime logic.
The public `ModuleInfo_t` declaration and PS2SDK loadcore's `boot_params` layout
establish the module records and entry handoff fields. AnyPS2 constructs these
data structures independently, using guest allocations and the supplied disc
archive; it does not copy a firmware initialization routine. The manager module
itself remains user-supplied data in ignored local storage.
PS2SDK `common/include/iop_mmio_hwport.h` identifies the SBUS configuration
register at 0x1f801450. PCSX2 `IopHw.h`, `ps2/Iop/IopHwRead.cpp`,
`ps2/Iop/IopHwWrite.cpp` and `R3000AInterpreter.cpp` identify ICFG, its retained
register storage and bit-3 PS1-mode use. The independent bootstrap device model
supports aligned word accesses and rejects switching into PS1 mode.
PS2SDK `iop/system/dmacman` exposes full-word get/set access to 0xbf801578 and
uses values one/zero for DMA enable/disable. The bootstrap device retains that
control state, consistent with the PCSX2 IOP hardware register storage path.
IOP DMA channel execution remains unsupported; no completion is fabricated.
PCSX2 `IopMem.cpp` suppresses main-memory writes when CP0 status isolation is
set. AnyPS2 independently applies that protection to its coherent diagnostic
backend, while rejecting isolated cache reads and cache swapping. Cache-control
configuration is retained for the entry flush sequence; cache-tag contents,
instruction staleness and cache timing are not modeled or claimed as verified.
The public PS2SDK intrman interface describes interrupt registration and CPU
suspend/resume contracts. PCSX2's pinned IOP hardware read/write paths and
R3000A exception/RFE handling were consulted for interrupt-register addresses,
acknowledgement, master read-to-disable, IP2 gating and exception state. AnyPS2's
controller and CPU integration are independently written; the tests use original
guest instructions. No firmware handler or intrman implementation is copied.
The pinned PS2SDK intrman export table, public declarations and kernel error
constants establish the partial native provider's ordinal/argument/error ABI.
The suspend/resume and disable contracts were checked against that open-source
service. AnyPS2 uses its own C++ handler storage and shared controller, with
original trampoline instructions and explicit unsupported results. This does
not claim to implement the full PS2SDK module or its DMA/dispatcher behavior.
The original registration IRX uses the public version-one ELF/IOPMOD layout and
loadcore's RegisterLibraryEntries ordinal. The metadata-first program-header
layout and absent module-ID sentinel follow that public loader interface. Its
entry, import call, result check and completion store are original instructions,
not extracted startup code. The processor revision value follows the pinned
PCSX2 R3000A reset state; its read uses AnyPS2's existing coprocessor load delay.
The pinned PCSX2 `IopMem.cpp` word-access paths describe the IOP SBUS register
view: shared F200/F210 addresses, opposite-side F220/F230 flag updates, F240
control toggles and F260 zero reads. The independent AnyPS2 adapter reuses the
existing SIF storage and rejects unsupported widths and control bits. Register
access alone does not assert DMA completion or successful reboot.
The public PS2SDK dmacman header/export ABI defines ordinal pairs, channel
numbers, transfer-start flags and the priority/enable operations. PCSX2's pinned
IOP hardware register declarations corroborate the two channel banks and tag
register locations. AnyPS2 independently maps these operations to shared C++
device state. No SDK implementation is copied; unsupported transfer starts and
DMA interrupt behavior remain explicit failures.
The pinned PS2SDK heaplib public interface/export table and its open-source
service define heap flags, return codes, eight-byte allocation accounting and
growth/release behavior. AnyPS2 uses an independently written map-based allocator
with host-owned metadata and real guest-sysmem backing, rather than copying the
linked-list implementation or a proprietary heap layout. Private chunk-helper
exports are not implemented. The Play! heaplib reference was inspected but its
placeholder-handle implementation is not used.
Startup planning uses the public import/export version fields and loadcore's
major/minor version organization. Its minimum-minor and ordinal checks are a
conservative static policy, not a complete implementation of loadcore's runtime
replacement rules. The public intrman callback declarations/ordinals establish
the context/preemption setter/resetter ABI; AnyPS2 stores validated pointers but
does not claim completed callback dispatch.

PS2SDK `libcdvd-common.h`, `libcdvd-rpc.h`, `libcdvd.c` and
`cdvdfsv.c` define CD/DVD initialization modes, server 0x80000592 and the
four-word reply. AnyPS2 accepts initialization only with a validated mounted
ISO, tracks initialized/exited state, and reports zero module versions/debug
flags rather than impersonating proprietary modules. Sector commands remain
unsupported; initialization is not a fabricated successful read.

The local reference checkouts under `third_party` are ignored. Game images,
extracted executables, generated guest code, and runtime traces stay under the
ignored `work` directory. The generic implementation and tests contain no GT4
instruction blobs or address-dependent game patches; the tests use synthetic
instruction fixtures.

The IOP timer register addresses, widths, mode fields, prescalers and polling
flags were checked against the pinned PCSX2 IopHw.h, IopCounters.cpp and
ps2/Iop/IopHwRead.cpp/IopHwWrite.cpp references. The implementation and synthetic fixtures are independently written. Timer
interrupt flag/rearm, pulse/toggle, target reset and primary interrupt-bit
semantics also use those public references. External-clock modes remain
explicitly unsupported.

IOP DMA interrupt control uses the pinned PCSX2 IopHw.cpp and
ps2/Iop/IopHwWrite.cpp register/event semantics and public PS2SDK intrman
interrupt numbers, mode bits and enable/disable ABI. Original C++ shares one
controller between MMIO and native service calls; no transfer or firmware code
is copied.

Incremental reboot scheduling uses the public PS2SDK iopcontrol reset protocol
and sifman SIFINIT polling contract. The EE transport-ready flag belongs to the
implemented EE driver initialization; it does not stand in for IOP readiness.
DMA halfword access follows the register widths in the pinned PCSX2 IOP hardware
access reference, using original width-merging code.

The guest SIF1 destination-chain receiver uses the public tag masks, register
updates and completion contract described by the pinned PCSX2 Sif1.cpp and
PS2SDK sifman sceSifSetDChain. Its bounded FIFO transfer implementation and
synthetic instruction fixtures are original. Timing remains diagnostic.

CD/DVD initialization register widths, status bits, NOP completion, parameter
error and result-FIFO semantics use the pinned PCSX2 CDVD.cpp and
CDVD_internal.h public device definitions. The implementation is original and
does not copy firmware identity, NVM, keys or drive-firmware contents. Media
commands remain unsupported rather than returning fabricated data.

The RTC response layout and UTC+9 device convention use the same pinned public
CDVD reference. Time comes from the host system clock, not firmware or saved NVM.

IOP partial-word byte selection was checked against the pinned PCSX2
R3000AOpcodeTables.cpp instruction definitions. The implementation reuses
AnyPS2's existing unsigned byte-merge helper, with original IOP delayed-result
handling and synthetic byte-oracle/pair tests.

Mechanical-controller query 0x03/0x00 returns the public PCSX2 fallback register
profile 03 06 02 00 (DEFAULT_MECHA_VERSION in the pinned CDVD.cpp). These four
interface-identification bytes are documented compatibility metadata, not a
firmware image, firmware implementation, console identifier, key or NVM dump.
AnyPS2 reports unsupported commands explicitly regardless of this profile.

CancelPOffRdy uses the public command number and response contract from PCSX2
and PS2SDK cdvdman; the ready-bit state belongs to the native device model.

Resource directory discovery and file-entry layout were checked against the
pinned PS2SDK iop/fs/romdrv/src/romdrv.c public interface. The bounded builder
is original and reuses AnyPS2's archive validator. Default directory bytes are
generated metadata only; no BIOS instructions, firmware files or ROMVER contents
are included.

The thread-yield syscall number, saved-frame layout and callback contract use
the pinned PS2SDK intrman and threadman public interfaces (intrman.c, thcommon.c
and thcommon.h). AnyPS2 implements an original C++ state transition and executes
the user-supplied guest scheduler; it embeds no firmware exception handler.

Primary interrupt acknowledgement, handler return semantics and preemption
callback ordering were checked against the pinned PS2SDK intrman public interface.
The native dispatcher and dedicated-stack state management are original C++;
no exception-vector code or firmware bytes are installed.

CpuInvokeInKmode uses the pinned PS2SDK intrman header contract and public syscall
argument mapping. The C++ wrapper records and budgeted guest call mechanism are
original; no firmware handler instructions are embedded.

DMA cascade ordering, acknowledgement and handler-result enable behavior use
the pinned PS2SDK intrman public device interface. Pending-channel selection
reuses AnyPS2's DMA device state rather than duplicating register semantics.
