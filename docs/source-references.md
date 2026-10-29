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
  `iop/system/sysclib/include/sysclib.h` supplies the public sysclib 1.1
  memory-function signatures and import ordinals. The native memcpy/memmove/memset bindings use
  those ABI declarations with standard host memory functions; no PS2SDK memory
  function implementation was copied or linked.
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

IOP SIF0 source-chain mode and tag fields use the pinned PS2SDK sifman interface
and PCSX2 Sif0.cpp/Sif.h public behavior, including partial-quadword trailing
lanes. The bounded sender implementation and synthetic echo program are original.

Native IOP memory import binding uses the major-version compatibility rule in the
pinned PS2SDK loadcore public source (`aLinkLibEntries` and
`compLibMinVersion_major_1`). Only the three implemented sysclib ordinals bind;
minor-version compatibility does not advertise a complete library. The original
C++ resolver and aggregate diagnostics copy no upstream implementation. Synthetic
major-1 minor revisions and incompatible major versions cover the binding policy.

Native allocation service ordinals, modes and query flags use the pinned PS2SDK
`iop/system/sysmem/include/sysmem.h` public ABI. Allocation granularity, failure
returns and block-query behavior were checked in the corresponding public
`sysmem.c`. AnyPS2 uses an original host C++ ordered-extent allocator; no upstream
allocator implementation, metadata layout or firmware code is copied or linked.

Native library registration/release uses the pinned PS2SDK public loadcore header
ordinals, registration-version and release contracts in `loadcore.c`, and library
error constants in `iop/kernel/include/kerr.h`. The compiled-descriptor validation,
host registry and service composition are original C++; no guest linked-list
implementation, stub patching or firmware code is copied or linked.

Native import binding and range-based link/unlink use the pinned PS2SDK loadcore
public ordinals and library error constants. Host descriptors replace instruction
patching and guest linked lists; the bundle compiler, transactional binding and
native transfer driver are original implementations. Test modules are synthetic.

Native loader query ordinals, the `iop_bootmode_t` public layout and query contracts
use the pinned PS2SDK loadcore header/source. Boot records are host-supplied data;
no firmware globals or implementation are copied. The Play! reference currently
returns a fixed null boot query; AnyPS2 instead implements configurable bounded
records and explicitly identifies the standalone probe's empty context.

Native ioman file calls use the pinned PS2SDK ioman.h ordinal/signature declarations
and io_common.h read-only flag. The host adapter reuses AnyPS2's original ISO9660
parser and file-handle logic extracted from its FILEIO RPC service. Only synthetic
ISO fixtures are committed; the user's commercial image and local read evidence
remain outside tracked source.

Native interrupt ordinals, signatures, permitted handler modes, suspend/resume
state and error values use the pinned PS2SDK intrman public header/source and
kerr.h. The native event domain, pending-cause queue, compiled callback discovery
and continuation snapshot are original C++; no exception-vector, controller or
firmware implementation is copied or installed.

Native event services use the pinned PS2SDK threadman/thevent.h public ordinals,
flags and structure declarations, thevent.c observable event/context contracts,
and kerr.h error constants. Host event storage, checked output and compiled
service integration are original C++; no guest scheduler, handle encoding or
upstream implementation is copied or linked. Only synthetic fixtures are used.

Native cooperative thread services use pinned PS2SDK thbase.h ordinals and
thbase.c sleep, wakeup, identity and cancellation contracts, with kerr.h errors.
The host continuation map, ready queue, idle interrupt integration and budget
accounting are original C++; no guest scheduler structures or upstream context
switch implementation are copied or linked.

Scheduler-backed event waits use the same pinned public thevent contracts for
FIFO waiter matching, clear-on-success, single-waiter exclusion and deletion
errors. The opaque continuation interface and native queue integration are
original implementations; guest saved-register pointers are not used as queues.

Native worker creation/start/deletion uses pinned PS2SDK thbase.h descriptor,
attribute and ordinal declarations and public thbase.c validation, alignment,
stack-fill/clear and dormant-state contracts. Native stacks and continuation
setup use AnyPS2's original allocator and scheduler; no guest register-frame or
kernel implementation is copied or executed.

Explicit native thread exits use the pinned public thbase ordinal and lifecycle
contracts: ordinary exit becomes dormant; exit-delete defers destruction until
execution has left the worker. AnyPS2 performs this transition after compiled
service retirement and reuses its original checked stack-release path.

Native StartThreadArgs uses the pinned public thbase.h signature/ordinal and
thbase.c argument-count, argument-pointer and null-source contracts. AnyPS2 uses
bounded native stack storage and a checked host copy, not the reference guest
register-frame construction. Oversized argument blocks stop explicitly.

Native GetSystemStatusFlag uses the pinned PS2SDK thbase.h/thbase.c stable-handle
contract and thcommon.c public initialization behaviour: a multi-waiter event,
zero initial bits and an optional bit selected by boot-mode record 4. AnyPS2
constructs this through its original native event service using supplied context;
no firmware initialization code or banner is copied or executed.

Native alarms use pinned PS2SDK thbase.h ordinals/signatures and the public
thbase.c/thcommon.c interval, duplicate/cancel, minimum-delay and callback-repeat
contracts. The default tick frequency follows the public conversion ratio.
AnyPS2's host clock, pending-alarm storage and compiled callback integration are
original C++; no hardware timer implementation, interrupt vector or firmware is
copied or executed.

Native outbound SIF transfer services use pinned PS2SDK sifman.h descriptor and
callback declarations, public sifman.c completion/status behaviour and exports.tab
ordinals. The supported aligned subset uses original direct host copies and
compiled completion queues. No DMA tag generator, hardware register implementation,
firmware code or proprietary packet payload is copied or linked.

Native SIF coordination uses pinned PS2SDK sifman.h/c readiness and command-address
contracts and sifcmd.c SIF1 completion/rearm usage. Status publish/acknowledge
semantics were cross-checked in pinned PCSX2 HwWrite.cpp and IopMem.cpp. AnyPS2
implements original native link state and receive notification, with no MMIO bank,
DMA engine or proprietary initialization code copied or linked.

Native diagnostic output uses the pinned PS2SDK stdio.h printf signature and
export ordinal, the existing IOP O32 register/stack calling convention, and
standard supported C integer/string formatting rules. The bounded parser,
formatter and host sink interface are original C++; no upstream formatter or
proprietary diagnostic strings are copied into the project.

Native SIF teardown follows the pinned public sifman_deinit contract: stop both
transfer directions and disable/release SIF0 while retaining the initialization
marker. AnyPS2 implements lifecycle changes through its own native transfer and
interrupt services. Reconnection is explicitly unsupported in the current subset;
no hardware shutdown sequence or proprietary code is copied.

Native SIF command registration/delivery uses pinned PS2SDK sifcmd-common.h
packet and callback declarations, exports.tab ordinals and public sifcmd.c table
selection/removal contracts. The original native implementation uses validated
RAM table access and the shared compiled callback scheduler. It does not copy
firmware, packet payloads, upstream dispatch code or DMA/interrupt machinery.

Native outbound commands use the same pinned public sifcmd declarations and
_sceSifSendCmd argument/header/ordering contract. The bounded peer mailbox,
transaction validation and byte-copy implementation are original C++. No
upstream DMA descriptors, hardware code or commercial packet contents are copied.

Command completion variants follow pinned public sifcmd exports 28/29 and the
SIF completion callback ABI. AnyPS2 reuses its original native transfer commit,
token and callback queue for both raw data and command sends, without copying
upstream interrupt or DMA implementations.

Native command-session semantics use the pinned public sifcmd.c InitCmd,
Get/SetSreg and control-message contracts, sifcmd-common.h packet layouts, and
sifman.h CMDINIT flag. AnyPS2 reuses its original event/scheduler and native link
services; it does not copy the upstream module bootstrap, firmware, banners or
interrupt dispatcher.

Unified inbound routing follows the previously recorded public command handler
and system-table selection contracts. Its owned mailbox, deferred-error handling
and integration with the original native callback scheduler are new AnyPS2 code;
no PS2 interrupt dispatcher or commercial packet data is copied.

Native RPC registration follows pinned sifrpc-common.h public queue/server layouts
and signatures and sifrpc.c registration/removal contracts. The native maps,
ownership checks and compiled-function discovery are original AnyPS2 code; no
upstream RPC allocator, proprietary server implementation or game data is copied.

Server-side RPC bind replies use pinned sifrpc-common.h Bind/End packet layouts
and public sifrpc.c service-search and bind-response field contracts. The original
AnyPS2 router retains a native reply snapshot under backpressure instead of
copying upstream packet pools, retry alarms or RPC implementation code.

Native RPC Call admission and GetNextRequest follow pinned sifrpc-common.h call
and server-record layouts and public sifrpc.c request-queue/wakeup contracts.
The owned payload snapshots, descriptor protection, native FIFO and scheduler
integration are original code; no upstream RPC execution or proprietary handlers
are copied or used as fixtures.

Native ExecRequest follows the pinned public SifRpcFunc_t signature and sifrpc.c
command-notified reply contract. The service-call continuation, bounded reply
snapshot and backpressure handling are original AnyPS2 mechanisms. No upstream
RPC execution wrapper, proprietary handler or firmware code is copied.

Native InitRpc uses the same pinned public sifrpc.c initialization sequence,
sifcmd-common.h RPCINIT register index and system-status event contracts. Its
session publication state and native retry-yield continuation are original C++
implementations; no upstream packet pool or proprietary startup code is copied.

Native RpcLoop follows the pinned public sifrpc.c GetNextRequest/ExecRequest/
SleepThread ordering contract. It reuses original native queue ownership and
continuations instead of copying the upstream loop or executing guest kernel code.

Native ChangeThreadPriority uses the pinned public thbase.h ordinal/signature,
priority range and thbase.c current-thread, priority-zero and rescheduling
contracts. The implementation updates the existing original native ready list;
no upstream kernel scheduler implementation is copied.

Native ReferThreadStatus follows pinned thbase.h's foreground status layout and
public thbase.c lifecycle/wait-field contracts. Creation metadata and wait IDs
come from original native scheduler records; the interrupt-only register context
is not exposed. Restart priority follows the public thread-start contract.

Native DelayThread uses pinned thbase.h ordinal 33 and public thbase.c delay-wait,
minimum-alarm and wakeup contracts. Original host-clock deadlines replace the
console alarm callback implementation; no upstream kernel code is copied.

Native semaphore semantics use the pinned public thsemap.h ordinals/record layouts,
thsemap.c count, waiter, deletion and status contracts, and kerr.h error values.
The provider and shared native wait interface are original C++ implementations;
no upstream kernel lists, handles or scheduler code are copied.

Native mailbox semantics use pinned thmsgbx.h ordinals, packet/status layouts,
thmsgbx.c queue and receiver ordering contracts, and kerr.h error values. Native
vectors own messages and waiters; the original implementation mirrors public
links without copying upstream kernel list or scheduling code.

Native Kprintf uses the pinned public sysmem.h ordinal/signature and sysmem.c
callback-routing contract. The existing original bounded C++ formatter and host
sink replace text output; callback-specific null-format controls are unsupported.

Native GetOtherData uses pinned sifrpc-common.h receive-descriptor layout and
sifrpc.c ordinal-23 arguments, completion and NOWAIT contracts. An original bounded
native copy replaces request/response latency when the peer owner supplies its
memory; public completion metadata is written without guest packet allocation.

Native getstat uses pinned io_common.h legacy record layout, iox_stat.h legacy
mode bits and public cdvdman recording-date conversion contracts. The existing
original ISO parser retains directory-record date bytes; native metadata encoding
is original code and does not depend on proprietary disc-driver implementation.

Native directory imports use pinned ioman.h ordinals and io_common.h's legacy
io_dirent_t layout. Original ISO directory parsing and shared metadata encoding
provide entries; the implementation owns native snapshots/cursors and never
copies a proprietary directory driver or exposes its host data structures.

Read-only mutation dispatch uses pinned ioman.h ordinals and ioman.c's write-access
check before calling a device. The native ISO mount reports standard EBADF,
EROFS and ENODEV failures according to its explicit read-only device policy.

Native lseek64 uses the pinned iomanx exports.tab legacy ioman 1.4 ordinal 30 and
iomanX.h signed 64-bit signature, with O32 even-register-pair argument/return
layout. Original checked arithmetic serves both native seek widths; it does not
copy an upstream driver implementation.

Native ioctl/ioctl2 dispatch uses the pinned PS2SDK iomanx exports.tab and
iomanX.c descriptor-validation contract, common/include/cdvd-ioctl.h command
identifiers, and iop/cdvd/cdvdman/src/cdvdman.c ordinary-file ioctl2 failure and
unknown ioctl failure contracts. The implementation is original native dispatch;
no upstream driver code is copied. Known hardware controls remain unsupported.

Native AddDrv/DelDrv use the pinned public PS2SDK ioman.h descriptor/operation
layout and ioman.c legacy 16-slot, initialization-failure and teardown-result
contracts. iomanX.c documents rejection of concurrent lifecycle calls. Original
C++ ownership, LLVM callback discovery and saved native continuations replace the
console manager implementation; no source or binary driver is copied.

Compiled driver files use the pinned ioman.h 16-byte file structure and operation
signatures, plus ioman.c device/unit path parsing, open result normalization,
read access checking and unconditional close cleanup contracts. Descriptor
ownership, native heap integration and LLVM callback invocation are original
implementations. The fixture driver and file contents are original synthetic data.

Compiled write/seek dispatch follows the same pinned ioman.h operation table and
ioman.c io_write access-bit and io_lseek origin-validation contracts. Native
buffer checks and shared continuation handling are original C++; no proprietary
file implementation or data is included in the synthetic regression.

Legacy compiled directory calls follow pinned ioman.c io_dopen/io_dread/io_dclose
contracts, ioman.h operation slots, and io_common.h's 300-byte io_dirent layout.
iomanX.c and iox_stat.h establish the distinct extended layout. Native directory classification and bounded output validation
are original code, and the compiled enumeration fixture is synthetic.

Compiled-driver wide seek follows the pinned iomanX.h lseek64 operation signature
and slot, iomanX.c extended-device/origin checks and signed error contract, and the
O32 aligned argument/return pair. Original native continuation code preserves the
wide result; the synthetic callback contains no commercial implementation.

Extended directory conversion follows pinned ioman_sbv.c's ioman_dread/statx2stat
field-selection contract, iomanX.c modex2mode access/type mapping, and iox_stat.h
alignment/layout. Original native encoding preserves the legacy private word;
zero-initialized scratch and unchanged EOF/error output avoid propagating
uninitialized wrapper storage. No upstream implementation is copied.


Compiled ioctl/ioctl2 dispatch follows the pinned public iomanX.c handle/type
checks and iomanX.h operation slots/signatures. Original native validation and
continuation code invokes compiled callbacks; synthetic commands and instruction
fixtures are original and contain no commercial driver implementation.


Compiled path dispatch uses the pinned public ioman.c io_remove/path_common
contracts and ioman_sbv.c's legacy mkdir default of 0755 for extended drivers.
Native record ownership, retry handling and fixtures are original implementations;
no filesystem driver or commercial instruction data is copied.


Native devctl uses pinned iomanX.c/iomanX.h's extended-device check, temporary
file record and seven-argument operation contract. O32 argument relocation and
buffer validation are original native code, exercised by synthetic callbacks.


Extended chdir/sync/umount dispatch follows pinned iomanX exports.tab, xx_dir,
iomanX_umount and handle_result contracts: extended type checks, operation slots,
flag forwarding, success normalization and negative errors. Native ownership and
synthetic callbacks are original implementations.


Compiled readlink follows pinned iomanX_readlink and iomanX.h's extended-device,
four-argument callback and unnormalized-result contract. Buffer validation,
continuations and the bounded synthetic link callback are original code.


Rename/symlink routing follows pinned iomanX.c xx_rename and iomanX.h operation
signatures, including relative destinations, cross-device/unit rejection and
result normalization. Bounded parsing, native ownership and synthetic callbacks
are original implementations.


Native GetDevType follows pinned iomanX_GetDevType's descriptor lookup, EBADF and
device type contract, with public iomanX.h FS/FSEXT flags. The native ISO service
uses extended filesystem flags (also used by the public cdrom driver). Metadata
ownership and validation are original native implementations.


Mount dispatch follows pinned iomanX_mount/iomanX.h's extended-device requirement,
six-argument callback, optional opaque arguments and result normalization. Native
stack preparation, bounds checks and synthetic callbacks are original code.


Native look_ctype_table uses the pinned public sysclib exports.tab, ctype.h flags
and nonstd/ctype.c ASCII-domain contract. Classification is an original arithmetic
implementation; no upstream table bytes or proprietary code are copied. The
undefined non-ASCII table access is rejected explicitly.


Native prnt follows pinned sysclib.h's callback/va_list signature and the public
sysclib eyalroz_printf/printf.c prnt wrapper's 513-flush/512-reset sequence and
return count. No formatter source is copied: the original bounded native formatter
is reused with a distinct argument reader and compiled continuation delivery.
Snapshot ordering and unsupported formats are documented explicitly.


Native strcmp/strlen/strncmp use the pinned public sysclib export signatures and
standard unsigned-byte comparison, termination and count contracts. The bounded
reader and native loops are original implementations, with synthetic strings.


String-search imports follow pinned sysclib exports.tab and public strchr/index,
strrchr/rindex byte-search signatures. The bounded native scan is shared with the
original length/comparison implementation; no upstream string implementation is
copied.


Native strcpy/strncpy use pinned sysclib exports.tab and the public C string-copy
contracts. Bounds validation, explicit overlap rejection and the shared native
copy implementation are original code with synthetic test strings.


Native bzero and _wmemset follow pinned sysclib exports/signatures and the public
nonstd/_wmemset.c contract: byte count, complete aligned word stores and original
destination return. The native fill/validation implementation is original; no
reference loop or table is copied.


Native sprintf follows pinned sysclib exports/signatures and the public C
formatting return/termination contract. It reuses AnyPS2's original bounded
formatter and O32 argument reader; no upstream formatting implementation is copied.


Native strcat uses the pinned sysclib export/signature and C append/return
contract. The implementation composes AnyPS2's existing bounded length and copy
code with synthetic strings; no upstream implementation is copied.


Native strtol follows the pinned sysclib Makefile's default USE_SIZE_OPTIMIZED=0
selection and public embeddedartistry_libc/stdlib/strtol.c behavioral contract,
including binary prefixes, saturation, end-pointer rules and absent errno writes.
The alternative size_optimized variant differs and is documented as such.
Implementation is original bounded parsing with widened intermediate arithmetic;
no upstream algorithm/source is copied or adapted.


Native reboot-mode metadata follows pinned loadcore exports.tab and
SetRebootTimeLibraryHandlingMode's mask-6 update/error contract. Original native
registry validation replaces firmware linked-list handling; no reboot success or
policy application is synthesized.

Non-auto-link registration follows the pinned public loadcore
RegisterNonAutoLinkEntries, aLinkLibEntries and RegisterLibraryEntries contracts:
mode bit 0 excludes a provider from import linking while leaving queries available.
The native registry and synthetic lifecycle fixtures are original implementations;
no firmware lists or upstream implementation code are copied.

CpuInvokeInKmode follows pinned public intrman.h and the intrman syscall-handler
contract for forwarding three register arguments and returning the callback result.
The host continuation and event-delivery hold are original native implementations;
no exception-handler assembly is copied into the product or test fixtures.

Native heaplib uses the pinned public heaplib.h/export ABI and heaplib behavior
for creation flags, block growth, allocation units, free-space accounting and
release results. Implementation reuses AnyPS2 native allocator code and host-owned
containers; no upstream allocator code, structures or instruction bytes are copied.
Synthetic tests describe the supported API boundary, not proprietary heap internals.

CheckThreadStack uses the pinned public threadman thbase.c/thcommon.c contracts
for current-thread stack distance, interrupt-context zero and the 168-byte low
stack threshold. The native ownership checks and diagnostic stop are original;
reference stack-frame details and firmware scheduling paths are not reproduced.

TerminateThread follows the pinned public threadman thbase.c lifecycle and
error contracts. Native wait cancellation replaces intrusive console queues with
an original explicit ownership interface; no firmware scheduling implementation
is copied. Active-callback/current-thread restrictions are documented.

Boot-callback phase, priority, GP and result handling follows the pinned public
loadcore AddRebootNotifyHandler and startup callback loop, plus the public
BootupCallback_t ABI. Host-owned queues, scratch allocation and compiled
continuations are original implementations; no firmware loop or assembly is copied.

Pre-connection local SIF publication follows the pinned public sifman
sceSifGetSMFlag/sceSifSetSMFlag interface, which has no peer-readiness precondition.
The native service retains published local state without creating a peer.


EE LLVM PADDUW lowering uses the pinned public PCSX2 MMI operation contract:
four independent unsigned 32-bit sums saturate at the maximum lane value. The
implementation maps this behavior to LLVM's vector unsigned saturating-add
intrinsic; no upstream implementation is copied. Scalar HI/LO transfers and their
secondary-bank counterparts use the public 64-bit transfer contracts. Shared
state offsets and original synthetic operands verify the native implementation.


EE LLVM MTSAB/MTSAH/MFSA follow the pinned public PCSX2 opcode contracts,
cross-checked against Play!'s EE instruction lowering. The references use different
internal shift units: AnyPS2 stores the byte count observed by MFSA, with MTSAH
producing an even byte count. Direct MTSA is not added in this milestone. MTC1/MFC1
follow the public raw 32-bit transfer and integer sign-extension contracts;
no upstream execution code is copied and no host floating-point conversion occurs.


Finite EE LLVM addition reuses AnyPS2's original integer arithmetic implementation,
with public FPU.cpp accumulator destinations and status-flag contracts informing
the adapter. Extended numeric ranges remain explicitly unsupported. SYNC uses
native LLVM fence semantics for the public ordering boundary; console pipeline
and cache implementations are not copied or linked.


FCR31 writable condition/status bits follow the pinned Play! COP_FPU.cpp mask;
CFC1 sign extension is consistent with the public PCSX2 transfer contract.
Implementation/revision selectors are intentionally unsupported because the
references expose different processor identities. LQ/SQ's address rounding and
full-register transfer contract were checked in the pinned public PCSX2 opcode
implementation; native LLVM vector accesses reuse AnyPS2's existing bounds checks.


Native EE SetupThread uses the pinned public PS2SDK startup argument order and
syscall number. The implementation reuses AnyPS2's existing initial-stack planning
policy and provides an explicit empty launch context. The native descriptor,
compiled-code protection and budgeted continuation adapter are original; no BIOS
thread structures or proprietary setup implementation are reproduced.


Native SetupHeap and EndOfHeap use the pinned public PS2SDK syscall numbers and
heap-start/size interface. The implementation reuses AnyPS2's existing bounded
heap-to-main-stack policy through a shared extent planner. It owns metadata only;
no BIOS allocator or proprietary memory-management implementation is copied.


EE LLVM EI/DI privilege conditions and EIE status-bit ownership were checked
against the pinned public PCSX2 COP0.cpp and R5900.h contracts. The cooperative
pending-event boundary is an original native adapter; it does not copy interrupt
scheduler code or BIOS exception handling. Signed/unsigned comparison widths and
sign-extended immediate contracts were checked in R5900OpcodeImpl.cpp. LLVM
lowering and synthetic comparison/event fixtures are original implementations.


Conditional moves and scalar shift widths/count masks were checked against the
pinned public PCSX2 R5900OpcodeImpl.cpp contracts. Integer likely-branch nullification
was checked in Interpreter.cpp; only the observable branch contract informs the
original LLVM control-flow lowering. No upstream interpreter code is linked into
converted output. Tests use original synthetic instructions and operands.


LWC1/SWC1 raw register transfers, floating comparison condition-bit ownership,
exponent-zero treatment and BC1 conditions were checked against the pinned public
PCSX2 FPU.cpp interfaces. AnyPS2's native comparison support deliberately rejects
exponent-255 operands instead of adopting the reference interpreter's clamping
approximation. LLVM lowering reuses original checked-memory and branch helpers;
no upstream execution implementation is copied.


CFC2/CTC2 control selectors, synchronization flags, scalar transfer widths and
integer-zero handling were checked in the pinned public PCSX2 VU0.cpp interface.
AnyPS2 routes these operations to an original typed native service contract rather
than linking a VU interpreter or copying its register/reset implementation. The
special CFC2 random-register transfer remains outside supported conversion scope.


LQC2/SQC2 register and VF0 transfer contracts were checked in the pinned public
PCSX2 VU0.cpp. The current native transfer path explicitly requires 16-byte
alignment; it does not claim the integer quadword rounding rule for VU transfers.
MOV.S/ABS.S/NEG.S bit operations and current overflow/underflow flag clearing were
checked in FPU.cpp. Storage and LLVM access lowering are original and reuse the
existing bounds/protection path without linking device execution code.


Disabled performance-control transfers and counter selector behavior follow the
pinned public PCSX2 COP0.cpp and R5900.h contracts. AnyPS2 does not adopt cycle
count approximations: active measurement is explicitly unsupported. Integer
multiply/divide widths, sign extension, divide-by-zero and overflow results were
checked in R5900OpcodeImpl.cpp. LLVM lowering and synthetic fixtures are original.


Finite scalar multiplication and vector add/subtract reuse AnyPS2's existing
original integer-significand calculations and documented finite-domain policy.
This change extracts pure helpers for both native output and diagnostic callers;
it does not copy upstream execution code or link the diagnostic runtime into
converted executables. Extended numeric domains remain explicit unsupported cases.


VMOVE/VMR32 selected-lane and rotation contracts were checked in the pinned public
PCSX2 VUops.cpp. LDL/LDR aligned-word merge behavior was checked in
R5900OpcodeImpl.cpp. Native lowering uses original LLVM vector operations and
bounded shifts while reusing AnyPS2's storage, bounds and alias helpers.


SDL/SDR merge direction and preserved-byte behavior were checked against the
pinned public PCSX2 R5900OpcodeImpl.cpp. Status visibility and system metadata
transfer widths were checked in COP0.cpp; PLZCW's two signed-word counts were
checked in MMI.cpp. Native implementations use shared LLVM merge lowering and
compiler intrinsics without importing upstream runtime code.


Signed arithmetic overflow contracts were checked against the pinned public
PCSX2 R5900OpcodeImpl.cpp; AnyPS2 reports an explicit unretired native stop rather
than implementing a BIOS exception path. Packed-copy ordering and halfword
replication were checked in MMI.cpp. LLVM overflow intrinsics and vector shuffles
provide the original lowering without copying upstream instruction execution.


Packed equality, color component bit positions, and byte/halfword/word
packing/interleave order were checked against the pinned public PCSX2 MMI.cpp.
MULT1 identification at the next conversion boundary was cross-checked against
R5900OpcodeTables.cpp. New lowering uses original LLVM comparisons, masks and
shuffles with synthetic bit/lane fixtures.
