# Source references and provenance

## Original PS2 test toolchain

- PS2DEV `v2.0.0`, commit `b1eb649a3765bf68500cfba9bb62433c526c90a8`:
  https://github.com/ps2dev/ps2dev/tree/b1eb649a3765bf68500cfba9bb62433c526c90a8
  The Windows release archive `ps2dev-windows-latest.tar.gz` has SHA256
  `bab98ee210c62aa3bd98d9bdda2af3f92a628e6e3d63e4b52f243ae56e3eb4ce`.
  Its GCC 15.2.0 cross-compiler builds original test inputs. It is a development
  dependency, not an AnyPS2 output backend. GNU compiler/binutils and bundled
  libraries retain their upstream licenses; no toolchain binary is redistributed.
- Windows host dependencies come from the official MSYS2 MinGW32 repository:
  https://repo.msys2.org/mingw/mingw32/
  Local package downloads were checked against repository SHA256 records. Their
  manifests and upstream notices remain with the ignored toolchain installation.
- The previously pinned PCSX2 `PINE.cpp` public protocol supplies read-only
  memory and version messages. The independent Python test adapter was validated
  against PCSX2 `v2.8.2`; it does not copy or link emulator implementation.
  Its role is differential testing, never native-runtime fallback.

The test C++, entry assembly, link script, result protocol and expected values
are original. They contain no commercial-game code, SDK routines or BIOS logic.
No PS2SDK libraries or startup objects are linked into these initial tests.

The external reference's interpreter/recompiler matrix uses the public
`EmuCore/CPU/Recompiler` settings (`EnableEE`, `EnableIOP`, `EnableVU0`,
`EnableVU1`) defined in the pinned PCSX2 `Pcsx2Config.cpp`. Those settings affect
only the separately launched reference process, never AnyPS2's generated output.

## Homebrew integration candidate

`glampert/ps2-homebrew` was inspected at
[`602441a6877a3136709d6320664340d52e3027a1`](https://github.com/glampert/ps2-homebrew/tree/602441a6877a3136709d6320664340d52e3027a1).
Its framework and cube-demo source carries Guilherme R. Lampert's 2015 MIT
notice. The ignored reference checkout retains the original notices and
`LICENSE.txt`; no upstream implementation or demo asset is incorporated into
AnyPS2. Fonts, audio, models and included documents require their own provenance
review and must not be assumed to share the top-level source license.

The cube demo creates its cube geometry and checker texture procedurally, and
exercises rendering, controller input and timing. The local compatibility port
builds all ten translation units and links a PS2 ELF with the toolchain above.
It fixes declarations, obsolete cast assignments, current GNU assembler syntax,
missing inline-assembly memory clobbers, and controller access after failed
initialization. It remains PS2 source compiled to a PS2 input, not native game
C++ output. LLVM conversion and native execution render the rotating cube grid
and text through Vulkan; a bounded 20-second run ended only at its requested
deadline. A later native SDL controller integration ran for 120 seconds; the
user confirmed DualSense camera movement. No ROM controller modules are supplied.
The source port binds the implemented native pad endpoints directly; its camera
logic is unchanged, including inverted vertical look.
The local build also recompiles the public SDK `graph.c` helper with targeted
CSR write-one-to-clear acknowledgements. Its original read/OR/write acknowledges
unrelated FINISH events when waiting for VSYNC, producing a race when native GPU
work has already completed. The port writes only the intended sync flag; AnyPS2
retains normal acknowledgement semantics and does not invent another FINISH.
See [the testing ladder](ps2-test-programs.md#larger-homebrew-tests).

## Conversion format and rendering references

The pinned PS2SDK `ee/rpc/cdvd/src/scmd.c` supplies the public disc-type and
drive-status RPC method numbers and four-byte response shapes. The native
implementation reports the explicitly configured ISO mount and synchronous
storage state; no optical-drive timing or firmware is synthesized.

ELF `.vutext` classification uses the section convention recorded in PS2SDK's
[`tools/srxfixup/src/eefixconf.c`](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/tools/srxfixup/src/eefixconf.c).
These VU payload containers can share an executable LOAD with EE code. AnyPS2
retains their bytes and reports their separate conversion requirement; an EE
function-pointer candidate does not make their contents EE instructions. The
implementation is original and does not copy the upstream linker configuration.

The Vulkan sprite batch implementation follows Khronos' public
[synchronization rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html),
[`vkCmdCopyBuffer`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer.html)
and [`vkCmdUpdateBuffer`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdUpdateBuffer.html)
contracts (accessed 2026-11-01). Ordered compute/transfer barriers retain
per-draw texture snapshots and palettes; one host upload/readback surrounds
each consecutive draw batch. No Khronos sample implementation is copied.

## Offline library-call mapping

DEV9 initialization uses the public export ABI and nonresident failure contract
in PS2SDK `iop/dev9/dev9/include/dev9.h` and `src/ps2dev9.c` at the same pinned
revision below. The independently written native owner reports
`MODULE_NO_RESIDENT_END` because AnyPS2 has no expansion network/storage provider;
it neither registers a working adapter nor invents register values. No driver
implementation was copied. Existing module lifecycle code handles the failure.
The absent-device path error follows `iop/system/iomanx/src/iomanX.c` at that
revision: path-based calls return `-ENODEV` before inspecting operation buffers
when no driver owns the device. Native mounts remain explicit namespace owners.

The inventory ABI catalogue records public library names, versions, ordinal
numbers and function names from the PS2SDK headers at
`2a18d15da1332665451a9bcf9a0d4df9029c9490`, including
`ee/kernel/include/syscallnr.h` and the IOP public import declarations. Each
catalogue entry retains its header path. These are interface facts, not copied
implementations, and never provide a runtime replacement by themselves.
PS2SDK's applicable file notices and Academic Free License 2.0 remain at the
[pinned source](https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490).
No PS2SDK implementation is linked. Unknown ordinals and private interfaces
remain unnamed; a matching public name does not prove behavioral equivalence.

The native byte-search/comparison batch uses the public sysclib `memchr`,
`memcmp` and `bcmp` signatures and ordinal declarations. The pinned public
implementations establish that comparison returns the difference of the first
unequal unsigned bytes and that `bcmp` shares that result. AnyPS2 independently
implements bounded spans using `std::memchr` and `std::mismatch`; no loop or
library source is copied. The bounded `strlen` fast path uses the same host
byte search while retaining its existing fault and scan-limit behavior.

Open-source native-provider candidates researched on 2026-11-01:

| Candidate | Pinned reference | License | Candidate use |
| --- | --- | --- | --- |
| SDL | `bd9f6ca9ff0e9509f49980fc1b70837f54b6f244` | zlib | Controller input and haptics behind an ABI adapter |
| Asio | `bdf337ddb1a667c274372a602de5857ad3cb3ed0` | Boost Software License 1.0 | Socket operations and asynchronous completions |
| libusb | `987c1170d0018ba82557823a98ae443e8335e11b` | LGPL 2.1 or later | Explicit physical USB transfers, subject to host-driver access |
| miniaudio | `877ea4a7c2601c7b0f2a44e83dfac42e6470fea6` | MIT-0 or public domain | PCM output, mixing and resampling; not an SPU2 implementation |
| Vulkan Memory Allocator | `0f1a757da579831283413295cc7f3b47eb521c77` | MIT | Vulkan buffer/image allocation; not GS command semantics |

API research: [SDL gamepads](https://wiki.libsdl.org/SDL3/CategoryGamepad),
[SDL license](https://wiki.libsdl.org/SDL3/FAQLicensing),
[Asio networking](https://think-async.com/Asio/asio-1.36.0/doc/asio/overview/networking.html),
[Asio license](https://think-async.com/Asio/License),
[libusb asynchronous transfers](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html),
[libusb](https://libusb.info/), [miniaudio](https://miniaud.io/), and
[VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator).
These are researched candidates only; this mapping does not download, copy,
link or enable them. Existing native services remain in use. Any integration
must retain the selected project's notices and prove the adapter's ABI,
ownership, ordering, error and completion semantics first.

AnyPS2 uses public instruction/ABI interfaces and local open-source
references. No BIOS code or BIOS disassembly is incorporated into this implementation.
A user-supplied BIOS may be used locally with a reference emulator for differential
diagnostics; it is not an AnyPS2 conversion input or output dependency.

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
16-bit QWC storage and CHCR start bit. Native packet consumers handle supported
VIF/GIF submissions. Unimplemented IPU, SIF-send and scratchpad transfer endpoints
accept only zero initialization; configuring or starting work stops explicitly,
and initialization never produces completion notifications.
PS2SDK `ee/dma/src/dma.c` also initializes the offset-0x80 scratchpad slots on
these channels to zero; that initialization is accepted, while nonzero setup stops.
Direct 128-bit VIF/GIF writes use the same native packet consumers. The public
`Vif_Codes.cpp` MSKPATH3 flag and GIF STAT M3P bit define the independently
implemented path-3 mask; masked submissions stop without consuming packets.
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
apply the reference's game-specific workaround. The scalar extended-range
implementation is described below; bounded VU paths still reject that domain.
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
The same pinned `GSState.cpp` Write/transfer-buffer logic bounds IMAGE writes to
the declared rectangle and discards excess data after completion. This informs
the independent upload planner's padding handling; no reference code is copied.
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
The LLVM IOP backend independently implements BLTZAL/BGEZAL, including the
unconditional link write and delay slot. Source-register r31 is rejected as
unpredictable, following the [MIPS instruction-set manual, revision 2.00](https://gamingdoc.org/wp-content/uploads/2020/07/mips_instruction_set_reference.pdf).
No manual text or reference implementation is incorporated into generated code.
An unbounded store through a fixed slot may call any instruction compiled into
that program, and every other address stops as unmapped code; proved loaded
pointers stay on their smaller set. A path that skips the load does not keep
another path's delayed pointer, and a known-address store decoded in another
bundle module uses that same classification. A callee discovered only from a
loaded pointer is discarded once that slot is open. A constant-bounded memory
fill, copy, or move poisons the addressed span rather than every slot in its
module. An unknown-address store can name a slot in any module, so every
single-slot load in the bundle stays open unless a stack-relative address, a
known address, or a constant span excludes that write. A store reached only
through that pointer still leaves the call open. The call may use any
instruction already compiled into the program.
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
PS2SDK `common/include/iop_mmio_hwport.h` identifies the SIO2 block at
physical 0x1f808200 and the SBUS configuration register at 0x1f801450. Compiled
word accesses use a session-owned native serial service. SEND configuration,
control reads, reset writes and interrupt acknowledgment are supported. START,
FIFO and response-status operations stop before retirement; no completion or
peripheral response is invented. The public hardware reference
https://psi-rockin.github.io/ps2tek/#sio2registers (consulted 2026-10-31)
identifies START/reset bits and FIFO direction. The pinned PCSX2 `SIO/Sio2.cpp`
and `SIO/SioTypes.h` were consulted for interface bits only; no code was copied.
Generated LLVM returns typed access metadata rather than retaining global device
state. PCSX2 `IopHw.h`, `ps2/Iop/IopHwRead.cpp`,
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
DMA interrupt behavior remain explicit failures. Compiled imports bind the same
register, priority and enable ordinals when a full device set is supplied.
The production service context supplies only the two priority words, so
priority and enable ordinals retire and channel, control, and transfer
ordinals stay unsupported. sceSetSliceDMA and sceStartDMA bind as
unavailable stops and do not report a completed transfer.
PS2SDK `thbase.h` declares `USec2SysClock` (ordinal 39) and `SysClock2USec`
(ordinal 40). The native alarm service converts those values with its
configured tick frequency and accepts both thread and interrupt context.
A second count that does not fit in 32 bits stops before either output is
written. `cdvdman.h` ordinal 24 is `sceCdReadClock`. The native provider
writes the eight-byte `sceCdCLOCK` from the host clock in Japan standard
time, BCD, with status and pad clear, and returns 1. A year outside
2000–2099 stops before the write. Other cdvdman ordinals stay unsupported.
`secrman.h` ordinals 4 and 5 store the memory-card command and device-id
handlers, and a zero argument clears the stored pointer. Those handlers are
not called. Ordinal 6 returns 0 because no card is authenticated. Download
and card-data ordinals stay unsupported. No PS2SDK implementation is copied.
The same pinned PS2SDK `timrman.h`, `vblank.h`, `thbase.h` and public service
implementations were consulted for timer handle/interrupt identifiers, callback
ordering/removal, and interrupt-context status fields. New native implementations
use monotonic time and the existing compiled callback dispatcher; no implementation
code was copied. Timer gate/external-clock/toggle modes remain explicit unsupported
requests. Display callbacks use the native graphics owner's nominal display edges,
not physical monitor timing. Suspended thread register-frame exposure is not yet
implemented and stops rather than returning a fabricated pointer.
The same pinned `xtimrman.h` and `timrman.c` define handler/setup/start/stop
ordinals 20 and 22–24 and callback-return repeat periods. These use native
monotonic deadlines and compiled callbacks; unsupported external clocks/gates
still stop. The pinned CD/DVD header and `sceCdStop` define stop completion
reason 6; native file reads complete synchronously while drive status and queued
completion remain explicit state. No reference implementation code was copied.
The pinned public threadman `thcommon.c` saves/restores each continuation's
interrupt-delivery state and initializes new workers and idle with delivery
enabled. Native scheduling retains this ownership without a hardware register
bank, so a sleeping worker's critical section cannot mask another worker.
The pinned `libcdvd-rpc.h`, `libcdvd.c` and `cdvdfsv.c` identify the file-search
server and 292/296/300-byte request layouts. Native searches use the mounted ISO
directory and validate both result destinations before publishing either.
Second-layer searches remain unsupported; no directory record is synthesized.
The same public NCMD read packet uses 2048-byte ISO sectors, an EE destination,
edge-fixup record and progress destination. Native copies support unaligned
destinations directly and publish zero deferred-edge lengths before completion.
Raw/audio-sector patterns remain unsupported; read failures retain an error code.
The same pinned vblank source documents ordinals 4–7, edge versus phase event
bits, multi-waiter semantics and base-handler priority. The independent native
implementation reuses AnyPS2 event objects and scheduled continuations; it does
not expose the reference kernel's private structures or copy its implementation.
The pinned PS2SDK heaplib public interface/export table and its open-source
service define heap flags, return codes, eight-byte allocation accounting and
growth/release behavior. AnyPS2 uses an independently written map-based allocator
with host-owned metadata and real guest-sysmem backing, rather than copying the
linked-list implementation or a proprietary heap layout. Private chunk-helper
exports are not implemented. The Play! heaplib reference was inspected but its
placeholder-handle implementation is not used.

The same pinned PS2SDK LIBSD export table, `libsd-common.h` and initialization
interface were consulted for public ordinals, two cores of 24 voices, and voice
reset on both initialization modes. No sound-hardware implementation is copied.
Windows output uses the Windows SDK XAudio2 API, following Microsoft's
[initialization contract](https://learn.microsoft.com/en-us/windows/win32/xaudio2/how-to--initialize-xaudio2)
and [mastering-voice contract](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2-createmasteringvoice).
The backend creates a stereo 48 kHz output and stopped mono PCM voices. It does not
interpret sound instructions. The pinned public LIBSD parameter, routing,
effect-workspace and callback interfaces also inform the native sound services.
PCSX2's pinned SPU2 volume and mixing definitions were consulted for signed
15-bit fixed volume, signed 16-bit input gain, mixer gate bit meanings and
core-zero-to-core-one routing. No emulator mixer or effect algorithm is copied.
Native gains use XAudio2's documented
[channel matrices](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-setoutputmatrix)
and [volume and pitch units](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-volume-and-pitch-control).
Volume sweeps, active effects and unimplemented playback requests remain explicit
unsupported boundaries; configuring an inactive voice is not evidence of playback.
Native PCM streaming uses the public LIBSD block-transfer interface and its
`sceSdBlockTransHandler` signature. The callback supplies the next buffer and byte
count through two output pointers; an empty pair terminates the native stream.
The callback-mode flag is inferred from local call sites and remains separate
from the public double-buffer mode. No game addresses enter the implementation.
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

The native read-only archive service uses the same pinned ROMDRV public
open/read/seek/close contracts and unit/handle limits. It reuses the disc archive
parser and shared native descriptor namespace. Original C++ owns the file data,
cursors and lifetimes; no firmware image is mapped. Unsigned seek offsets clamp
to EOF as specified by that interface; negative read lengths are rejected with
EINVAL rather than relying on signed overflow or out-of-range copying.
Native ROMDRV replacement selection uses the pinned `romdrv.h` 2.x import ABI
and `exports.tab` six-entry layout. Mount/unmount use its documented error
numbers; startup activates an empty native service rather than probing firmware.
The explicit conversion choice is recorded in the local module manifest.
Native reset-session teardown ordering follows the pinned PS2SDK
`ee/kernel/src/iopcontrol.c` reset protocol: submission precedes clearing the
old SIFINIT/CMDINIT announcements. New native startup is scheduled after those
acknowledgements, without instruction-count delays or invented readiness flags.

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
The default 32 system-command slots follow the same public module's startup
contract. AnyPS2 owns these registrations as C++ data and dispatches them through
its existing compiled callback scheduler. Explicit registration or removal
overrides the corresponding native protocol handler; unrelated protocol services
remain connected. A caller-supplied system table replaces the default registry.

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
Native context startup also follows the module-start link initialization and
receive-buffer publication contracts. Initialization is explicit and owner-driven;
peer handshake, RPC readiness and reset completion remain separate transitions.

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

The native disc event-handle query uses the same pinned cdvdman.c's devctl
`0x4391` output contract and `sceCdSC(0xFFFFFFF5)` event identity, plus
iomanX.h's six-argument O32 devctl signature. The event is allocated through the
existing native event service after output validation. Querying it does not
signal completion or assert device readiness; asynchronous disc notifications
still require native producers. No CDVD register model or upstream driver code
is imported.

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
the adapter. Scalar extended ranges are covered by the later numeric upgrade
below. SYNC uses
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


## EE operation-family expansion

The pinned public PCSX2 R5900OpcodeTables.cpp and R5900OpcodeImpl.cpp supply
secondary multiply/divide, scalar multiply-accumulate, trap, branch-link, MTSA,
PREF and unaligned-word contracts. MMI.cpp supplies packed arithmetic, saturation,
permutations, accumulator packing, word products and halfword-product layouts.
PHMSBH's complemented odd products are preserved; unusual PMADDW/PMSUBW behavior
is left unsupported. No upstream execution implementation is copied.

COP0.cpp supplies ERET's ERL/ErrorEPC and EXL/EPC selection. Cache.cpp identifies
the supported data-cache hit operations; native coherent RAM lowers their ordering
to fences without a console cache model. VU0.cpp supplies full-width interlocked
QMFC2/QMTC2 behavior. VUops.cpp supplies macro multiplication, raw min/max,
absolute-value, fixed-point conversion and lane/flag ownership contracts.

FPU.cpp and x86/iFPU.cpp supply conversion, min/max, division and square-root flag
and rounding contracts. AnyPS2 reuses its original pure integer conversions and
division, and adds an original integer square-root calculation with explicit
rounding. Generated LLVM uses compiler arithmetic/intrinsics and independently
expressed lane maps. Fixtures contain only original synthetic programs; all
commercial input and derived output remain ignored.


Native memory initialization additionally uses the pinned PCSX2 `R5900.h` CP0
selector names and `Cache.cpp` operation classifications: DXLTG, DXWBIN, IXIN and
BFH. The native flat/coherent-memory replacement is AnyPS2's explicit contract,
not copied cache machinery. Its empty tags and rejected reservations are documented
in `ee-native.md`; synthetic fixtures contain no extracted commercial instructions.


Native EE semaphore service numbers and the six-word public structure follow the
pinned PS2SDK `ee/kernel/include/syscallnr.h` and `kernel.h`. The original native
owner uses bounded C++ storage, matching the existing project's established
nonblocking service behavior without linking its diagnostic kernel. Contended
scheduling and overflow return semantics are explicitly not supplied.


Native SetSyscall uses the pinned public PS2SDK syscall number and signature.
AnyPS2's original diagnostic kernel supplies the existing project contract for the
16-byte handler frame and EXL restoration. The new native implementation owns a
bounded table and frames separately and accepts only generated LLVM membership;
it does not import firmware tables or link the diagnostic interpreter.


Callback discovery is original bounded analysis of decoded instructions and ELF
segments. It uses public syscall wrapper conventions and the existing decoder;
no commercial addresses, table contents, instruction sequences or title-specific
rules are stored in the converter or fixtures.


Native registration metadata is original AnyPS2 service data. The public PS2SDK
`getkernel.c` confirms that software may inspect syscall-handler pointers; its
firmware-instruction address extraction is not implemented or copied. AnyPS2
reserves its own ELF-disjoint extent and publishes only registered compiled
callbacks, with explicit rejection of unsupported table modification.


Native timer addresses and configuration fields follow pinned PS2SDK
`common/include/ee_regs.h` and `ee/kernel/include/timer.h`. In particular,
0x10001810 is Timer 3 MODE, not a VIF register; earlier documentation has been
corrected. The monotonic host-clock implementation is original and does not link
the diagnostic SystemBus timer model.


Native cache services use pinned PS2SDK `kernel.h` FlushCache mode constants and
void signatures, plus `syscallnr.h` FlushCache/iFlushCache numbers. They share the
existing native coherent-memory contract, expressed as a host fence without
reconstructing console caches or installing RAM instructions.


Copied-payload mapping, destination discovery and exact-byte publication guards
are original AnyPS2 conversion mechanisms. Tests use original synthetic programs
and ELF fixtures. Commercial mapping files, expected-byte constants, LLVM and
compiled artifacts are private derived output under ignored `work/` only.


Native interrupt delivery follows the existing AnyPS2 interrupt-dispatch contract
for INTC line order, the Timer 3 alias, EXL/EPC entry and full interrupted-context
restoration. Public INTC address/bit definitions come from pinned PS2SDK
`ee_regs.h`. The native owner is independent of the diagnostic device scheduler;
no firmware handlers or register initialization tables are supplied.


Public native INTC services use pinned PS2SDK `syscallnr.h` and `kernel.h` entry
numbers and callback signatures, and the existing project's insertion, handle,
removal-count and negative-return chain contracts. Records and snapshots are
original bounded native C++ data structures, independent of the diagnostic kernel.


Native timer event latching and write-one-to-clear status follow the pinned
PCSX2 `Counters.cpp` public reference behavior and PS2SDK timer mode definitions.
AnyPS2 computes threshold crossings arithmetically from its monotonic host clock,
with original fractional-tick accounting and cooperative producer polling. It
does not adopt console cycle scheduling or a per-tick emulator loop.


Native EE thread lifecycle uses the pinned public PS2SDK `ee/kernel/include/kernel.h`
thread descriptor/status layouts and `syscallnr.h` ordinals, together with the
existing AnyPS2 startup stack reservation and thread-service contracts. The bounded
native record owner, cooperative continuation switching and descriptor candidate
analysis are original implementation. Tests use original synthetic instructions
and descriptors; no game descriptors or code are included in the repository.


Native application preferences use the pinned public PS2SDK `osd_config.h`
ConfigParam/Config2Param layouts and `kernel.h`/`syscallnr.h` void service contracts.
The application-owned settings container and validation are original AnyPS2 code.
Defaults are project policy, not recovered console configuration or firmware data.


Address-space instruction identities and CP0 parameter names were checked against
the pinned public PCSX2 COP0 implementation. The native request interface and LLVM
lowering are original code; they do not import its TLB implementation. No production
mapping provider is supplied by this milestone.


The transfer-completion status compatibility view uses pinned PS2SDK `ee_regs.h`
addresses and the existing AnyPS2 status acknowledge/mask-toggle contract. Its
producer-owned pending events and interrupt publication are original native code;
no DMA engine, firmware initialization state or transfer timings were copied.


The native EE SIF receive endpoint uses the existing AnyPS2 SIF service/control
contracts and public PS2SDK register and syscall identities. Direct whole-packet
copying follows the project's native IOP endpoint architecture; no diagnostic
FIFO, DMA stepping engine, BIOS initialization table or game-specific behavior
was copied. Peer readiness requires explicit native peer publication.


Native DMA handler services use pinned PS2SDK syscall identities, including
negative iEnableDmac/iDisableDmac, and the existing AnyPS2 ordered interrupt-record
contract. They reuse the original native INTC context/chain implementation with
separate registration domains and explicit native completion ownership.


Outgoing SIF batching follows the existing AnyPS2 public descriptor validation,
padding and transfer-status contracts. The snapshot owner and atomic native sink
interface are original; they do not reuse the diagnostic FIFO or DMA-step engine.
Tests use original data arrays and a native in-memory sink.


The native bootstrap command peer implements public SIF command-buffer and software-
register messages. Interfaces were checked against pinned PS2SDK `ee/kernel/src/sifcmd.c`
and the existing AnyPS2 native IOP command session. The parser, message-port owner
and connection to the EE batch sink are original C++; no firmware image, retail
startup code or proprietary initialization data is supplied.


RPC initialization replies follow the existing public SIF command protocol.
The bind-reply encoder was factored from original AnyPS2 native IOP RPC code and
is shared by both native paths. No server availability is invented. Initial
interrupt-class enablement is an explicit native application launch policy rather
than a recovered firmware register image.


Native application aliases reuse AnyPS2's existing `default_ram_address` policy;
public pointer conventions were checked against pinned PS2SDK `kernel.h`
UNCACHED_SEG and UCAB_SEG. Interrupt-safe SIF aliases follow pinned `syscallnr.h`.
No dynamic TLB model or firmware mapping table was imported.


Interrupt-safe semaphore syscall aliases were checked against pinned PS2SDK
`ee/kernel/include/syscallnr.h` and the public RPC completion usage in
`ee/kernel/src/sifrpc.c`. They reuse original AnyPS2 semaphore services. Bounded
registration-function summaries and their synthetic instruction fixtures are
original analysis code; no commercial function bodies are included.

The unavailable RPC identifier 0x80000001 is identified as the file-I/O service
by pinned PS2SDK `ee/kernel/src/fileio.c`. Probe diagnostics report the actual
request identifier rather than selecting runtime behavior from a game address.


The native FILEIO RPC adapter reuses the original AnyPS2 disc/file service and
public RPC field layout already used by its diagnostic transport. Payload/header
size and destination conventions were checked against pinned PS2SDK
`ee/kernel/src/sifcmd.c` and `ee/kernel/src/sifrpc.c`. The service registry,
preflight checks and direct native memory publication are original code. Tests
use a generated ISO containing original numeric data, not commercial game bytes.


Native GS lifecycle handling reuses the existing AnyPS2 public CSR/IMR bit
contract, with reset/flush mapped to real Vulkan device synchronization. It does
not import a firmware initialization sequence or claim a proprietary chip
revision. GsGetIMR/GsPutIMR and negative aliases were checked against pinned
PS2SDK `ee/kernel/include/syscallnr.h`. The shared Vulkan build helper and atomic
64-bit service adapter are original code.


Native display deadlines reuse the project's public NTSC/PAL phase convention,
with original rational monotonic-time arithmetic replacing diagnostic cycle
stepping. No firmware register tables or initialization sequence are supplied.
Tests use an injected host clock and original compiled interrupt fixtures.


The offline VU framing work consulted the already pinned PCSX2 public VIF command
and microVU encoding references for MPG count and paired instruction field layout.
The framing library and synthetic tests are original implementations; no upstream
execution or dispatch code is included. Private observed MPG data is kept only
under ignored work/ and must never become a committed test fixture.


## Native loader ABI references

The pinned PS2SDK `iop/system/modload/src/modload.c` also documents the separate
priority-8 module loader thread. AnyPS2 implements that ordering independently
with its existing native continuation scheduler and a module-completion wait.

Native modload ABI binding and callback ownership use the pinned PS2SDK
`iop/system/modload/include/modload.h`, `src/exports.tab`, and
`src/modload.c` registration/query contracts. The native owner stores only
validated AOT callback addresses; absent registrations remain null. It performs
no authentication or module loading. Selector 15 answers whether a path names a
device the native disc or archive opener can use: `cdrom:` and `cdrom0:`, or
`rom:` through `rom3:` without requiring a mounted image. Every other
well-formed path is illegal. A null or unterminated guest pointer stops without
success. The pinned SDK function returns success for every path and is marked
unofficial, so that stub is not used. LoadStartModule (ordinal 7) reads a path, its argument span, and an optional
result pointer before any file access. A null filename returns -201. A path
outside `cdrom:`, `cdrom0:`, and `rom:` through `rom3:` returns -201. A legal
path with no mounted image, and a missing cdrom file, return -203. An empty
image returns -204. None of these writes the result pointer. An opened cdrom
file is fingerprinted against the linked AOT catalog. A miss, and a hit that
has no sealed entry, stop without relocating the image, registering it, calling
an entry, or writing a module result. A sealed entry runs only when its
relocated image is already installed at the converted base and that entry is
compiled code. Conversion can mark a module `sealed@` so its code is compiled
and omitted from the startup image, startup entries, and source-hash catalog.
The relocated initialized bytes are written to `sealed.data`. An empty sealed
list does not read that file. A non-empty list is installed only at the
converted 256-byte-aligned base inside the native heap: the image hash must
match, the rounded span must not intersect the startup image or another sealed
module, and the heap hold must succeed. A base that does not fit is refused
and is not slid. The hold keeps ordinary allocation off that span until
LoadStartModule claims it. The loader reserves that range, passes argc/argv and a module
record, and transfers to the entry. After the entry returns, the caller's
result is the module id and the guest result word receives the entry value.
Entry value 1 releases the reservation. Any other value leaves the module
resident, and a second start returns -206. A sealed image that is not installed,
or an entry that is not compiled code, stops without a call. Nested
LoadStartModule while an entry is still running stops. `rom` bytes are not read
yet, so that device returns -203. StartModule (ordinal 8) takes the module id
in the first argument and its result pointer at `sp+16`. This owner has no
loaded-but-unstarted image, so a known resident or in-call id returns -206 and
every other id returns -202. Neither path reads the name, the argument span, or
the result pointer, and neither calls an entry. Other lifecycle imports still
stop at the service boundary. The pinned loadcore
`exports.tab` identifies FlushIcache/FlushDcache; immutable AOT code and coherent
native data replace cache banks, with a host publication fence at these calls.
No firmware cache routine or upstream implementation is copied.

## GT4 recompilation research reference

The native CD/DVD synchronous-command endpoint uses the previously pinned
PS2SDK `ee/rpc/cdvd/src/scmd.c`, `common/include/libcdvd-common.h`, and
`iop/cdvd/cdvdfsv/src/cdvdfsv.c` as public ABI references. ReadClock returns
native PC system time encoded in the eight-byte BCD clock layout, using JST
independently of the PC timezone. The implementation uses C++ chrono calendar
conversion; it does not reproduce firmware clock access. Dates outside
2000–2099 and unimplemented commands stop explicitly. The endpoint shares the
existing initialized ISO-session lifetime. No upstream implementation is copied.

Method 34 follows `sceCdMmode` in the same public sources, including
`iop/cdvd/cdvdman/src/cdvdman.c`. The native service stores media mode 1 (CD)
or 2 (DVD) on the initialized ISO session and returns success. Shutdown clears
the stored mode. Any other value, a missing session, or an invalid buffer stops
without a result write and without replacing a stored mode. Sector reads are
still required to honor the stored mode. No upstream implementation is copied.

Disk-ready servers `0x8000059A` and `0x8000059C` use the same public
`sceCdDiskReady` callback. With an open ISO and no transfer in progress, the
blocking mode and a ready poll return command-complete (2), and the status
query returns the idle-ready byte `0x40`. A poll against a closed session
returns not-ready (6), and its status byte is shell-open. No drive register
or busy-wait is reproduced.

The selected compiled IOP reset-session integration binds these endpoints.
Media-mode selection and one disk-ready poll are startup progress, not a
rendered frame or boot. The compiled IOP session then accepts a `cdrom` path
from the boot-device query above. The next stop is modload `LoadStartModule`
(ordinal 7). That call opens the cdrom image and fingerprints it. The image is
not a sealed compiled entry, so the call stops without relocating, registering,
or starting a module. No frame was rendered.

- Repository: https://github.com/moesuito/gt4-staticrecomp
- Inspected revision: `795cdc086464b003d49c0b44d0911dc761f58f5c`.
- Inspected documents: `docs/ARCHITECTURE.md`, `docs/IOP-MAP.md`, and
  `native/THIRD_PARTY.md`. The repository reports MIT for its own implementation;
  its dependencies have separate terms. No implementation was imported.
- Useful lead: the IOP map distinguishes native system-library replacements from
  disc-loaded game modules. Use this to investigate service boundaries rather
  than attempting to compile console kernel internals indiscriminately. Validate
  every proposed replacement against actual initialization and completion state.
- The inspected architecture and IOP map describe older interpreted subsystem
  paths; the newer README reports further AOT coverage. These documents do not
  establish an interpreter-free build. Reported gameplay and performance have
  not been verified locally.
- Architectural differences: generated C++, hardware models, game-specific
  integration, and a separate PCSX2-derived GS renderer. These are not a substitute
  for AnyPS2's LLVM-only, generic, BIOS-free native runtime and Vulkan renderer.
- Referenced donor: https://github.com/snuri00/ps2-web-recomp at
  `1b4808793a12c45125262fd2d5f50a9ec9f7c020` (MIT as recorded by the reference).
  Inspected `runtime/src/iop/sif.cpp` and `runtime/src/iop/io.cpp` for service
  organization. Its SIF reset assigns status flags directly; do not transplant
  that assignment as evidence of completed native startup.
- Next useful comparisons: reset service ownership, RPC server installation,
  disc resource loading, and offline discovery of VU uploads. Keep game-specific
  addresses, proprietary content and generated commercial code out of AnyPS2.

## God of War static recompilation reference

- Repository: https://github.com/KIexster/god-of-war-recomp
- Inspected revision: `2d3c91dc173727058073c267bbfb27b3dab703a1`.
- Repository license: GPL-3.0; preserve applicable licensing and attribution before
  adapting implementation. No implementation from this reference has been added.
- Local reference checkout: ignored `third_party/god-of-war-recomp/`.
- Useful sources: `tools/vu1/vu1n_gen.py`, `docs/ARQUITECTURA.md`, native runtime
  patches, and synthetic tests. The VU generator documents block identity,
  branch/delay boundaries, delayed integer branch operands and cross-block flag
  visibility. Its comments identify interpreter fallback and differential checking.
- Architectural differences: generated C++ rather than LLVM; VU1 focus rather than
  the current VU0 upload; game-specific overrides; OpenGL rather than Vulkan;
  optional native IOP with interpreter paths. Do not import those fallback paths
  or game-specific addresses into AnyPS2's production runtime.
- Next application: use its observed hazards and validation structure to guide
  original VU branch/Q lowering, upload identity checks and synthetic regression
  cases. A reference project's reported gameplay is not AnyPS2 compatibility proof.

Native VIF launch framing also references the pinned PCSX2 `Vif_Codes.cpp`
MSCAL, BASE, OFFSET and ITOP handlers and `Vif.h` public field layout. These
establish entry masking and launch-input/buffer-selection transitions. The new
implementation calls compiled functions transactionally; it does not adopt the
reference's execution queue, instruction executor or game-specific timing paths.

The V4 data conversion family uses the pinned PCSX2 `Vif_Unpack.cpp` as a behavior
reference for signed/unsigned lane widening and packed 5:5:5:1 colour expansion,
including the packed format's MODE independence. The native implementation uses
original bounded byte reads and synthetic payloads; no upstream unpack routine
or game payload is copied.

VCALL entry selection follows the pinned PCSX2 `COP2.cpp` and `VU.h` public
behavior: immediate microprogram entries and CMSAR0-selected entries. AnyPS2
lowers these operations to native launch callbacks and compiled functions; no
reference interpreter implementation is incorporated.

The pinned PCSX2 `Memory.cpp` public mappings identify the VU0 data window and
its 4 KiB mirrors, and the separate 16 KiB VU1 data window. AnyPS2 maps those data
accesses to native owner buffers during LLVM lowering. It does not expose the
reference's instruction-memory handlers or virtual-TLB implementation.

The native GIF adapter follows the already recorded public packet/register
interfaces. Sprite bounds and clipping were factored from AnyPS2's existing
original `gs_sprite.cpp` into a shared planner. The adapter supplies native draw
commands to the existing Vulkan renderer; it does not import a reference GS
machine or any commercial packet payload. GPU fixtures are original synthetic data.

DIRECT/DIRECTHL payload lengths follow the pinned PCSX2 `Vif_Codes.cpp` public
command behavior, including the zero-immediate 65,536-quadword case. AnyPS2 frames
bounded data and submits it synchronously to its native graphics consumer; it does
not adopt the reference's transfer queues, DMA stalls or timing model.

Native image upload planning uses the public transfer fields already implemented
in AnyPS2's original graphics code. The 32-bit destination address calculation is
shared between native planning and diagnostic graphics. Tests use original small
RGBA/RGB payloads; no game images or extracted texture data are included.

Sprite texture setup is factored from AnyPS2's existing original renderer into a
shared planner. The native packet adapter supplies fixed-UV endpoint/configuration
data to that planner and uses the existing Vulkan sampler. Synthetic tests contain
only original texels and commands.

Native indexed palette planning is extracted from AnyPS2's original
`gs_texture.cpp`, using the previously recorded public palette interfaces.
Both diagnostic and native consumers now share the planner. New tests use
original synthetic colours and index data; no upstream code or game palette
payload is incorporated.

The native PSMT4 upload address helper uses the same original bit-address layout
as AnyPS2's Vulkan sampler, based on the public references already recorded for
indexed textures. New upload fixtures contain original indices and independent
boundary-address expectations. No reference implementation or commercial image
payload was copied.

Native sprite alpha testing and blending use the already pinned PCSX2 public
`GSRegs.h` TEST/ALPHA/PABE/COLCLAMP field interfaces and the documented GS blend
equation. The original Vulkan implementation evaluates integer channel arithmetic,
explicit failure masks and destination tests; no upstream rasterizer code is
copied. Synthetic fixtures exercise comparison, rounding and saturation behavior.

Scalar and V2 native unpack conversion uses the pinned PCSX2 `Vif_Unpack.cpp`
as a public behavior reference for scalar replication and paired-lane repetition.
The implementation extends AnyPS2's original bounded widening helper. V3 remains
unsupported because its fourth-lane input/transport behavior needs separate
modeling; it is not approximated with a zero or repeated third component.

Native masked vector uploads use the same pinned `Vif_Unpack.cpp` public
selection behavior: input, row, column and preserved lanes, with cycle-group
saturation. The original data conversion helper now applies a typed mask snapshot;
no reference unpack executor is incorporated.

Skipped-cycle destination spacing and fixed row addition use the same pinned
public VIF unpack interfaces. AnyPS2 represents spacing as bounded destination
stride metadata and uses unsigned lane addition. Filling cycles and mutable-row
modes are explicitly rejected rather than borrowing an interpreter implementation.

MODE 2 accumulation and MODE 3 row replacement follow the pinned public VIF
write-selection behavior. The native implementation prepares original typed
write plans and final row values before submitting to a compiled vector owner;
no upstream unpack loop or executable instruction payload is incorporated.

Masked filling-cycle planning uses the pinned `Vif_Unpack.cpp` source-consumption
and mask-selection interfaces, cross-checked against the public PS2SDK VIF row
filling interface (https://ps2dev.github.io/ps2sdk/group__packet2__types.html).
Only row/column/protected fill lanes are accepted. Input-selected fill positions
would require source lookahead beyond nominal consumed data and remain rejected;
no guessed repeat value or upstream executor is used.

Masked V3 data conversion uses the pinned VIF public component layout and mask
selection interface. The fourth lane must explicitly select row, column or
preserved data; input-selected fourth lanes are rejected. No reference's
out-of-payload fourth-lane read is copied into the native planner.


Native MSCNT submission behavior was checked against the already pinned PCSX2
`Vif_Codes.cpp` public `vifCode_MSCNT` and `vuExecMicro` interfaces: reuse the saved
program position and perform the same TOP/ITOP and buffer selection updates as
MSCAL. AnyPS2 implements this through existing compiled-entry dispatch and its
transactional native submission owner. No upstream execution loop was copied.


Native host profiling-source enumeration uses Microsoft public Windows APIs:
`TraceQueryInformation` with `TraceProfileSourceListInfo` and `PROFILE_SOURCE_INFO`
(Windows SDK 10.0.28000.0 interface, consulted 2026-10-30). The original bounded
parser and query wrapper do not copy SDK implementation code.
- https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ne-evntrace-trace_query_info_class
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-enablethreadprofiling
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readthreadprofilingdata

The thread-profiling documentation requires a driver to configure hardware
counters and states that unconfigured counter slots read as zero. Such zeros
are not valid evidence of zero measured events and are not used as guest results.


The native counter investigation also consulted Microsoft's public ETW PMU
collection and system-session documentation on 2026-10-30:
- https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events
- https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session

The interval accumulator is original implementation over typed counter readings.
It is not derived from a third-party profiler, and synthetic samples are original.
Live ETW capture and the mapping to guest measurements remain unverified.


The original ETW counter-record adapter uses the Windows SDK 10.0.28000.0
`EVENT_RECORD`, PMC extended-data and processor-index interfaces, together with
Microsoft's documented Thread_V2/CSwitch layout (consulted 2026-10-30):
- https://learn.microsoft.com/en-us/windows/win32/etw/thread-v2
- https://learn.microsoft.com/en-us/windows/win32/etw/cswitch
- https://learn.microsoft.com/en-us/windows/win32/api/evntcons/ns-evntcons-event_header_extended_data_item
Only the public interface layout is used; no profiler implementation was copied.

The context-switch adapter also consults Microsoft's PerfView `CSwitchTraceData`
at revision `9f071a5cb2e70cdfee13abc510344ab1e8840c90` (MIT, copyright
.NET Foundation and Contributors), inspected 2026-10-30:
- https://github.com/microsoft/perfview/blob/9f071a5cb2e70cdfee13abc510344ab1e8840c90/src/TraceEvent/Parsers/KernelTraceEventParser.cs
- https://github.com/microsoft/perfview/blob/9f071a5cb2e70cdfee13abc510344ab1e8840c90/LICENSE.TXT
Its newer-version parsing retains incoming/outgoing thread IDs at offsets 0/4.
AnyPS2 uses those interface facts in its original adapter; no implementation
code is copied. Version 5 support is restricted to the observed 28-byte record;
only the common thread-ID prefix is consumed, not the additional payload fields.
Version 2 still requires 24 bytes. Other versions and lengths fail explicitly.

PCSX2 release `v2.8.2` was used as a separate, unmodified local reference
application for startup observations on 2026-10-30:
- https://github.com/PCSX2/pcsx2/releases/tag/v2.8.2
Its existing PINE protocol and debugger were used to collect private observations.
The already pinned source checkout's `SaveState.cpp` and `PINE.cpp` were consulted
for interface framing. No emulator, BIOS, game code or save-state content is
incorporated into AnyPS2. Reference execution is not native-conversion acceptance.

The counter-configuration change uses the already pinned PCSX2 `R5900.h` control
field interface and the separate emulator checkpoint described above. It does
not copy PCSX2's approximate counter accumulation or return its unmeasured cache
miss value as a native measurement. Unknown measurement state is an original
conservative implementation. Object-table discovery and its fixtures are original.

Native EE sleep/wakeup continuations use the already pinned PS2SDK thread status
and syscall interfaces, with the pinned Play! PS2OS sleep/wakeup/cancellation
behavior as a reference. The implementation is original and uses compiled native
continuations; no reference scheduler or BIOS code is copied or linked. The
CD/DVD initialization endpoint was identified from PS2SDK `ee/rpc/cdvd/src/libcdvd.c`
and `iop/cdvd/cdvdfsv/src/cdvdfsv.c`. Its original native initialization adapter
uses the public `CdInitPkt` layout in `ee/rpc/cdvd/src/libcdvd.c` and mode values
in `common/include/libcdvd-common.h`. It opens/closes an AnyPS2 ISO session and
returns the public legacy zero-version metadata; no proprietary module version,
firmware implementation or reference-emulator success stub is copied.


Counter selector handling was checked against the pinned PCSX2 `COP0.cpp`
public event classification and `R5900.h` control fields. The independent native
implementation recognizes explicit selector disable without adopting PCSX2's
virtual-cycle accumulation or unsupported-event zero approximation. Regression
fixtures execute original counter transfers compiled through LLVM.

The explicit native thread-cycle policy uses Microsoft's public Windows API
contracts, consulted 2026-10-31:
- https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-enablethreadprofiling

The original adapter measures owner-thread CPU cycles including native kernel
and user work, without frequency scaling. Windows warns against converting this
quantity to elapsed time. Hardware-counter profiling requires configured counters;
unconfigured zero readings are not accepted as measurements. No Microsoft
implementation code or additional driver is incorporated.

Live sampled PMU collection reuses the original AnyPS2 ETW adapter and interval
accumulator with the Microsoft ETW interfaces cited above. The owner-thread
same-ID scheduling boundary was observed in live Windows records; the accumulator
now closes and reopens that interval with the same measured counter baseline.
No event count is inferred from elapsed time. The collector uses an owned system
session, validates loss/error state and exposes only completed measured intervals.

Interrupt-context GetThreadId normalization uses the already pinned PS2SDK public
`ee/kernel/include/syscallnr.h` and `kernel.h` declarations. The IOP callback change
is original conservative AOT discovery of code-valued arguments escaping to
compiled libraries; it does not copy a module's implementation or patch a game
address. Synthetic fixtures are original.
`GS/GSRegs.h` SCANMSK and `GS/Renderers/SW/GSRasterizer.cpp` at the pinned PCSX2 revision establish scanline parity masking. The native sprite shader independently rejects masked rows before pixel work; reserved mode 1 stops explicitly. DIMX/DTHE affect only 16-bit targets, which remain unsupported, and fog-enabled primitives remain rejected.

At the existing pinned PCSX2 revision, GSRegs.h defines TEX2 and the display
configuration fields; GSRendererSW.cpp restricts interpolated color to lines and
triangles. SIO/SioTypes.h and SIO/Sio2.cpp supply the public serial packet fields,
missing-device status, response padding and completion cause. The original
native adapter has explicitly disconnected endpoints and synchronous native
completion, not a serial timing model. The pinned PS2SDK dmacman.h and dmacman.c
provide sceSetSliceDMA's five-argument ABI and success return. Native descriptors,
owned buffers, validation and typed LLVM transfer metadata are original AnyPS2
code. No commercial code, SDK source or game-derived fixtures are incorporated.

EE semaphore wake ordering was checked against the pinned Play! PS2OS.cpp
SemaLinkThread/SemaReleaseSingleThread behavior and PS2SDK EE wait-status fields.
The independent native continuation queue hands a token directly to the oldest
waiter and defers interrupt-context scheduling until interrupt return. No
reference game-specific stack modifications are used. The pinned PS2SDK IOP
sifman.c describes word-based source transfers; native SIF copies now accept
word-aligned source buffers while retaining complete-quadword length and EE
destination checks. Tail-lane and interrupt-tag behavior remains unsupported.

Retail EE semaphore maximum-count behavior was cross-checked against the pinned
Play! PS2OS.cpp SignalSema implementation and SP193's firsthand EE/TOOL behavior
report (2020-01-19):
https://www.psx-place.com/threads/newlib-porting-challenges.26821/page-2
The retail count can exceed max_count; that field remains observable metadata.
AnyPS2 increments the actual native count rather than saturating or discarding a
signal. Signed-count exhaustion remains an explicit unsupported boundary. No
firmware implementation, debug disassembly or reference game workaround is used.

Native EE idle identity follows the distinct idle-thread selection in the pinned
Play! PS2OS.cpp scheduler and its GetThreadId contract. AnyPS2 reserves handle 255
for the host idle continuation; it executes no console idle program. Unsupported
queries or mutations of that internal continuation stop explicitly. Interrupts
retain the interrupted identity until native scheduling resumes an application
thread. No upstream implementation was copied.

SBUS callback registration/removal uses the pinned PS2SDK kernel.h public ABI
and syscallnr.h numbers. The public ps2rd EE syscall notes describe sixteen
causes and a cause argument to each callback:
https://github.com/mlafeldt/ps2rd/blob/b433fa34bed0edb2665d5d6bd4f45585606aed74/Documentation/technical/ee-syscalls.txt
The implementation independently reuses the native interrupt continuation owner.
Native registration/removal returns zero after a completed slot update and -1
for an invalid cause; the public notes do not specify retail return values, so
exact numeric return compatibility remains unverified. SBUS callbacks are void
and do not use the INTC handler-chain return convention. Event injection requires
an explicit native producer; registration does not manufacture a bus event.

GetMemorySize follows pinned PS2SDK syscall 0x7f and Play! PS2OS.cpp
sc_GetMemorySize: it returns the configured application RAM size. The native
service validates its actual backing allocation rather than supplying a game
constant. Synthetic end-of-test sentinels use unimplemented syscall 0x7b.

The pinned PCSX2 Hw.h SIO_ISR/SIO_TXFIFO addresses and HwWrite.cpp byte-output
behavior identify the debug serial path. The native adapter sends bytes to an
explicit synchronous host sink; a completed write has no pending transmit queue.
An absent or failed sink stops. It does not model serial baud timing or receive
traffic. The CLI owns diagnostic output, not the reusable library.

The native thread lifecycle extends the existing scheduler with normal-context
TerminateThread, SuspendThread, ResumeThread and ReleaseWaitThread using the
public PS2SDK kernel.h/syscallnr.h interfaces at the pinned revision. Suspended
waiters remain suspended after an event or explicit wait release. Wait release
returns failure to the blocked operation; termination removes pending wait and
wake state before restart. Interrupt variants remain unsupported. No upstream
implementation is copied.

A complete indexed address-layout write sequence can be lowered as one native
layout declaration. The current lowering validates only writable identity RAM,
existing native RAM aliases and the already allocated scratchpad. It creates no
TLB/register bank and adds no lookup to loads or stores. Other layouts and
readback remain unsupported. The seven-operation sequence is a single native
boundary and needs at least seven operations of diagnostic budget; no partial
hardware-register state is exposed. Interior entry points still stop through
the unsupported primitive path.

- Zlib 1.3.1, https://zlib.net/fossils/zlib-1.3.1.tar.gz,
  SHA256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`:
  unmodified static decompression dependency of the conversion tools only.
  Notice retained in [zlib license](third-party/zlib-LICENSE.txt).
- Gran Turismo Modding Hub, revision
  `d70af9d3db86aa1fb0f91ff7ce7ee8ad719b4c61`,
  https://nenkai.github.io/gt-modding-hub/formats/ps2_core/:
  public compressed segmented-executable container fields inform the independent
  C++ importer. Only the unencrypted raw-deflate container is supported; no keys,
  game routines or game data are included. Filename and game identity are not
  used for parsing or execution selection. Opaque metadata is not authenticated.
  Imported ELF files now carry an original AnyPS2 `PT_NOTE` (name `AnyPS2`,
  type 1, four-byte version 1) recording the absence of source permissions.
  Direct control flow may reach any file-backed segment in those files; only
  discovered instructions receive code protection. Ordinary ELF permissions
  remain authoritative, and unrelated packed data is not treated as code.

- The pinned Play! `Source/ee/PS2OS.cpp` ExecPS2/SetupThread interfaces and
  PS2SDK public startup argument structures inform independent native image
  handoff: snapshot arguments, retire the old main continuation, clear process
  INTC callbacks, mask INTC delivery, and initialize the next main thread.
  No reference implementation or console instructions were copied.

- GCC 3.0.4 public manual, [Initialization](https://gcc.gnu.org/onlinedocs/gcc-3.0.4/gcc/Initialization.html):
  legacy constructor/destructor list framing (header, function pointers, zero
  terminator). Used as interface documentation for independent bounded AOT
  candidate recovery when ELF section metadata is absent. No GCC runtime code
  or manual text is copied.

The pinned PCSX2 R5900OpcodeTables.cpp and VUops.cpp interfaces were consulted for
VU macro opcode fields, clip history, Q/I operand selection, quotient flag
updates, outer-product operand ordering and the 23-bit random-register recurrence
(taps 4 and 22). Independent LLVM lowering reuses AnyPS2 numeric helpers and
shared vector ownership; no upstream execution loop or instruction interpreter
is copied into generated output.

At the same PCSX2 pin, `x86/microVU_Lower.inl` provides the DIV/SQRT (7) and
RSQRT (13) dependency distances. Independent LLVM lowering retains pending Q
across compiled blocks and publishes it when the scheduled distance has elapsed.

Native USB enumeration uses Windows SDK SetupAPI, `GUID_DEVINTERFACE_USB_HUB`,
`IOCTL_USB_GET_NODE_INFORMATION`, `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`
and `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` public interfaces:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_node_connection_information_ex
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_descriptor_from_node_connection
The pinned PS2SDK `iop/usb/usbd/include/usbd.h`, `src/exports.tab`,
`src/usbd_api.c` and `src/device_driver.c` describe the public LDD descriptor,
registration return codes and synchronous probe/connect ordering. Independent
C++ ownership replaces controller initialization and guest linked lists. No
reference implementation or commercial code is copied. Unsupported transfers
and topology changes stop; discovery never fabricates an empty host bus.
Control pipes now retain a Windows hub handle and verify the attached device.
Native HID endpoint ownership uses Microsoft's public
[HID architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture),
[USB port driver keys](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-_usb_node_connection_driverkey_name),
SetupAPI/Configuration Manager device ancestry, and overlapped Win32 reads.
Physical-device ancestry and interface number identify the endpoint; device IDs
do not select game behavior. Exclusive host access produces a failed public
pipe open, not a fabricated pipe or a successful empty transfer. The pinned
PS2SDK `usbd_api.c` and `device.c` define failed-open and seven-byte location ABI.
HIDAPI 0.15.0 is pinned at `dbff4ea89f55a572aeb0c53a7b32ea70853ec260`
([upstream](https://github.com/libusb/hidapi/tree/dbff4ea89f55a572aeb0c53a7b32ea70853ec260)),
using its BSD license in [hidapi-license.txt](third-party/hidapi-license.txt).
Windows exposes parsed HID capabilities, not the original wire descriptor.
HIDAPI reconstructs an equivalent report description from those capabilities;
AnyPS2 publishes that description and its corresponding length consistently in
the native USB snapshot. This is a native descriptor translation, not a claim
to preserve the device's original descriptor encoding. Unsupported collections
remain explicit; no generic report template or device-ID override is used.
LIBSD sample transfers use the pinned public `libsd-common.h`, `voice.c` and
`block.c` interfaces. Native transfer completion follows actual copies or
XAudio2 consumption, with compiled callbacks. Microsoft's
[source buffer lifetime](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-submitsourcebuffer)
and [voice state](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-getstate)
contracts govern audio ownership and completion. No console mixer is copied.
Standard descriptor reads use the documented
[USB_DESCRIPTOR_REQUEST](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-_usb_descriptor_request)
contract. Configuration selection adopts only a matching, currently active
Windows configuration; changing host device configuration remains unsupported.
The pinned `usbd_main.c` callback thread and `usbd_api.c` transfer interface inform
deferred foreground completion. Callbacks run as serialized compiled native
continuations with owned stacks, rather than interrupt callbacks or inline calls.

The pinned PS2SDK sysclib public interfaces define bcopy source/destination order,
word-copy byte-count truncation and string search/span signatures. Native LLVM
copy intrinsics retain bounds, code-write protection and pointer aliases. The
case-conversion imports take a byte and operate on ASCII letters. They lower
directly to native comparisons and arithmetic; unsupported high-byte inputs
stop explicitly without a locale-dependent conversion or a table lookup. The
cdvdman ordinal 22 interface identifies console iLink identity; it is deliberately
an unavailable native service, reported during conversion and stopped if called.
No console identity or successful response is invented.

The pinned PS2SDK CDVDMAN power callback interface stores a callback plus user
data and returns the previous callback. AnyPS2 maps it to an opt-in application
lifecycle: window close queues the compiled callback, and PowerOff requests exit
from the native program. The application consumes that request and releases its
native resources; no operating-system shutdown or console power model is used.
Hosts that do not consume application-exit requests leave these imports unavailable.

CDVDMAN DVD reads translate public second-layer logical sectors to raw ISO
offsets. The pinned PS2SDK `sceCdLsnDualChg` contract removes the 16-sector volume
prefix for reads starting on the second layer. PCSX2's pinned `CDVDisoReader.cpp`
locates that layer's descriptor at the first volume's sector count. AnyPS2 derives
and validates the boundary from the ISO, caches it for that open file, and keeps
raw converter reads unchanged. Unknown appended layouts stop without a guessed
offset; no game identity participates in the mapping.

The pinned PS2SDK `xmodload.h` defines module list/status/search ordinals and the
`ModuleStatus` data layout. Module entry results and starting/resident flags use
its public lifecycle contract. Independent C++ records track entries actually
started by AnyPS2; compiled-but-unstarted catalog entries are excluded. The
Play! module-list stub is not used. List outputs start at element zero and report
both written IDs and total matches, with output validation before any write.

Native continuation buffers follow the twelve-word public IOP `jmp_buf` size
and the register order described by Play!'s `Iop_Sysclib` at its pinned revision
above. No emulator implementation is copied. `longjmp` normalizes zero to one
as required by the C interface and accepts only compiled targets; abandoning
an owned native callback remains unsupported. The scheduler's priority and
ready-queue operations follow the pinned PS2SDK `threadman/src/thbase.c`
interfaces, including deferring interrupt-requested scheduling until return.

The pinned PS2SDK CD/DVD public headers and `cdvdman.c` describe sector-read
arguments, error codes, media/status queries, callback reasons and callback
delivery from interrupt context. Native IOP disc services share the already
initialized ISO session with the EE RPC service. Host file reads finish before
publication; completion callbacks are deferred through the existing compiled
callback scheduler. Optical seek/spin timing is not reproduced. The pinned
implementation's `sceCdSync` modes 0/1, 3–6 and 16/17 describe command and
RPC completion waits/queries. Accepted native reads complete synchronously;
these queries preserve separately queued callbacks. Streaming and the distinct
mode-32 event wait remain unsupported rather than reporting invented completion.
The pinned
PCSX2 `CDVDisoReader.cpp` documents the PS2 ISO convention in which the first
volume's block count points at the next volume descriptor. AnyPS2 independently
validates that metadata; an unrecognized layout is not reported as single-layer.

The pinned PS2SDK thfpool public header, threadman fixed-pool service and kernel
error definitions specify four-byte block stride, FIFO/priority waits, high/low
allocation, status fields and error codes. The independent native implementation
uses the existing bounded heap and continuation scheduler with host-owned free
lists; no kernel structures or allocator instructions are reproduced in data.
The pinned PS2SDK SIF RPC public structures and client routines specify bind,
call, reply and busy-state contracts. Native clients retain packet ownership
until the peer replies, deliver compiled callbacks with the caller's global
pointer, and resume waiting native continuations after callback completion.
Notification-free asynchronous calls remain an explicit unsupported boundary.
The pinned EE `sifrpc.c` execution routine writes the client and command in a
call-completion record; unlike bind replies, it does not echo the request's
packet address or transaction ID. Native completion correlates the owned live
client, validates output, and only then releases its callback/waiter. Unspecified
scratch fields in the peer's reply are not treated as transaction identifiers.

BC0F/BC0T and their likely forms use the selected-completion condition described
by `COP0.cpp` and `Dmac.h` at the pinned PCSX2 revision. LLVM emits a call to the
native completion interface followed by native conditional control flow. The
condition observes completed submissions selected by the existing submission
policy, independently of interrupt enables. It does not advance simulated DMA
or synthesize completion; a missing provider remains an unsupported boundary.

Sparse executable protection is emitted as bounded native comparison functions
with direct calls, grouped by image lifetime. This original generator change
shares checks across stores without a runtime address table. Imported packed
segments retain writable data gaps; only discovered instructions are protected.

Native packet interrupt boundaries follow `Vif_Transfer.cpp`, `Vif1_Dma.cpp`,
`Vif.cpp` and `Vif.h` at the pinned PCSX2 revision: an unmasked command interrupt
is delivered after that command, with INT and (except MARK) VIS exposed until
stall cancellation. The native implementation retains an owned packet remainder
and resumes it after the compiled handler acknowledges the boundary. Graphics
fences finish prior work before notification; submission completion is withheld
while data remains. It does not simulate FIFO occupancy, transfer cycles or bus
arbitration. Reads of intermediate source positions are unsupported while a
native batch is suspended. This is an independent implementation of the public
behavior, not copied emulator execution code.
The `Hw.h` and `Vif.h` public layouts at that revision distinguish NUM at
offset 0x60, CODE at 0x80 and ITOPS/BASE/OFST/TOPS/ITOP/TOP at 0x90 through
0xe0. The native adapter exposes completed command metadata at those offsets;
compiled notification handlers must read the command that raised the event.

USB-IF HID Device Class Definition 1.11, sections 6.2.2.7 and 8, defines report
IDs and global Push/Pop state:
https://www.usb.org/sites/default/files/hid1_11.pdf
The original native descriptor composer joins Windows collections only when
their input/output/feature report identities are disjoint, and restores globals
between collections. Feature/output requests select the owning collection.
Physical access denial is reported from CreateFileW; no partial endpoint or
synthetic report is substituted. Fully accessible multi-collection input streams
and overlapping report layouts still require an implementation.

Native reverb uses the Windows SDK `XAudio2CreateReverb` and
`ReverbConvertI3DL2ToNative` APIs and their built-in room presets:
https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-xaudio2createreverb
https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-reverbconverti3dl2tonative
https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-seteffectchain
The pinned PS2SDK `libsd-common.h` and `libsd/src/effect.c` define the public
mode numbers, signed stereo depth, workspace sizes and clear flag. Mode names
map to native room, padded cell, living room, auditorium, concert hall, arena
and stone corridor presets for room, studio 1/2/3, hall, space and pipe. This
is a deliberate audible modernization, not sample-exact console reverb.
Preset changes replace the native effect and clear its history; the clear
operation also replaces it through XAudio2 rather than accessing an attached
APO from another thread. Variable echo/delay modes remain unsupported. Original
synthetic impulse tests process and clear each native effect offline; service
tests execute LLVM-generated imports and verify routing and failure handling.

The pinned PCSX2 `GSState.cpp` display circuit calculations document the
half-height framebuffer read when both SMODE2 interlace and frame mode are set.
The native scanout independently applies that source-row bound and duplicates
each source row for progressive window presentation. It retains the configured
display extent and buffer origin. This explicit line-pair reconstruction does
not implement temporal field blending. Aligned constant-alpha dual-circuit
composition is described below.

ELF section classification follows the System V generic ABI, ELF Object File
Format 4.3 draft, section 3 (consulted 2026-11-01):
https://gabi.xinuos.com/elf/03-sheader.html
Allocated sections without SHF_EXECINSTR remain data even inside an executable
LOAD segment. AnyPS2 validates their mapped extents before using them to reject
false callback candidates. SHT_NOBITS uses the memory extent rather than file
storage. This is an independent implementation of the file-format metadata.

The pinned PCSX2 `GSState.cpp` vertex submission and software rasterizer are
behavioral references for triangle lists, strips, fans, suppressed drawing kicks,
last-vertex flat color and top/left edge inclusion. The native Vulkan triangle
path independently assembles three vertices and evaluates integer edge functions
on the GPU. It reuses native framebuffer, alpha-test and blend operations; no
upstream rasterizer code is incorporated. Shaded color uses integer barycentric
interpolation with truncation, without reproducing internal GS interpolation
precision. Fixed UVs use the same integer interpolation and existing native
texture sampler. ST/Q triangles retain address/data and packed GIF Q-latch
ordering and use native floating-point interpolation before perspective division.
This does not reproduce internal GS interpolation precision. Non-finite or extreme
coordinates and triangles crossing zero Q are rejected before GPU submission.
Fog and fixed-fragment triangles remain explicit unsupported boundaries.

The same revision's `GSLocalMemory.h`, `GSRendererSW.cpp` and
`GSDrawScanline.cpp` supply public depth-format, comparison, alpha-failure and
coverage-write behavior. The original Vulkan implementation preserves 32/24-bit
depth, unsigned never/always/greater-or-equal/greater comparisons, color/depth
masks, and suppressed depth writes on partially covered edges. Color and depth
surfaces that overlap are rejected. Native edge antialiasing uses a 4x4 sample
grid and covered-sample centroid interpolation; it is a deliberate modernization,
not reproduction of GS edge hardware or its interpolation precision. Sprite
clears accept FIX because sprites have no edge-coverage interpolation. No
upstream rasterizer implementation is incorporated.

The pinned PS2SDK `graph_mode.c` adjacent-row flicker filter and the pinned
PCSX2 `GSRegs.h` PMODE layout and Vulkan merge shader describe constant-alpha
dual-display composition. AnyPS2 independently combines aligned circuits and
background color; displaced circuits and dynamic alpha remain unsupported.

The pinned PS2SDK `fileio-common.h`, `loadfile-common.h`, `sifrpc-common.h`,
`ee/sbv/include/smod.h` and `iop/kernel/include/kerr.h` define bootstrap public
interfaces. Native standalone startup exposes an empty module-list record,
real host clock/console services and KE_NOFILE for absent ROM modules. This
does not claim controller availability or create successful module-load stubs.
`_SifSendCmd` leaves the old dsize field when no extra payload is sent; the
native receiver validates the actual extra-payload destination and packet size.

Defined ELF function roots follow the generic ABI symbol-table contract:
https://gabi.xinuos.com/elf/05-symtab.html
The opt-in function-symbol pass accepts defined STT_FUNC entries in validated
executable sections, excludes undefined weak symbols and records its roots
separately. Metadata coverage is not proof of complete pointer-flow discovery.

The pinned PCSX2 `x86/microVU_Macro.inl` CTC2 status-register case documents
replacement of sticky bits 6-11 while preserving current arithmetic flags 0-5.
AnyPS2 implements that mask independently in the existing native VU owner;
compiled transfers wait for ownership and never run an instruction decoder.

The pinned PS2SDK `freesd.c` address API selects the core from bit 0 and voice
from bits 1-5, independently of bits 6-7. The native selector decoder preserves
those aliases. Address writes discard the low four byte-address bits; the
effect-end selector instead retains the inclusive end of a 128 KiB region.
Compiled synthetic imports verify aliases, readback, alignment and bounds.

## Native controller input

SDL at `bd9f6ca9ff0e9509f49980fc1b70837f54b6f244` is linked as a static,
input-only native provider. Its zlib notice is retained in
[SDL-license.txt](third-party/SDL-license.txt) and copied beside native builds.
The archive SHA256 is
`6d820fec9af8b0a9d1fe386dd9c41a401df0a564ea941570140391da765b36c3`.
SDL owns device discovery and controller mappings; AnyPS2 independently adapts
its standardized buttons and signed axes to the public legacy libpad RPC ABI.
The protocol reference is PS2SDK `ee/rpc/pad/src/libpad.c` at the pinned revision
above: old command identifiers, 64-byte double-buffer records and active-low
button packets. No ROM module, firmware, or upstream pad implementation is copied.
Unsupported commands stop explicitly. Digital buttons do not claim pressure
sensitivity. Vibration capability comes from SDL gamepad properties, and the
legacy actuator alignment/direct commands map to `SDL_RumbleGamepad`. Small
binary vibration uses the high-frequency channel; large variable vibration uses
the low-frequency channel. Host API failures remain unsuccessful replies. Native
300 ms leases are refreshed while the application runs and cancelled on close;
disconnection clears motor ownership before another device can take the slot.

The local homebrew input port binds the implemented native RPC endpoints directly
instead of loading ROM controller modules. This is an explicit source adaptation
compiled into a PS2 ELF and then LLVM AOT, not a successful ROM-load stub.

The controller-enabled camera also reaches scalar MADD/MSUB product underflow.
PCSX2's pinned `x86/iFPUd.cpp` (`FPU_MUL`, `recMaddsub`, `ToPS2FPU`) supplies
an independent behavioral reference: round and flush the product before the
addition, retain sticky underflow from that stage, and derive current underflow
from the final addition. AnyPS2 reuses its own integer-significand arithmetic
and adds a separate scalar ABI helper; the conservative VU contract is unchanged.
No upstream implementation is copied. The later scalar extended-range upgrade
covers exponent-255 and overflow cases; this is not a claim of hardware-exact
multiplication.

## Native audsrv sample playback

The original native provider uses the public PS2SDK audsrv RPC contracts and
APCM container fields at revision `2a18d15da1332665451a9bcf9a0d4df9029c9490`:
[`audsrv_rpc.c`](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/ee/rpc/audsrv/src/audsrv_rpc.c),
[`iopheap.c`](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/ee/kernel/src/iopheap.c),
and `tools/adpenc/src/adpcm.c`. The audsrv implementation is LGPL-2.0; it is an
interface reference, not copied into the native provider. The local demo links
the public EE client and retains its upstream notices under ignored output.

ADPCM predictor coefficients, sample saturation and loop-flag semantics were
checked against PCSX2 `pcsx2/SPU2/Mixer.cpp` at
`1f67c53b6723fcd6a90041a5b296f0e34a6a3222` (GPL-3.0+). No implementation was
copied. The original bounded decoder supports mono APCM version 1; unsupported
formats and loops requiring continued predictor history are rejected explicitly.
Decoded samples use the existing XAudio2 backend with native resampling, not
console interpolation or an SPU register model.

### Scalar extended-range arithmetic

The pinned PCSX2 `pcsx2/x86/iFPUd.cpp` (`ToDouble`, `ToPS2FPU_Full`,
`recDIVhelper1`, `recMaddsub`) is the public semantic reference for the EE
scalar exponent-255 finite range, signed saturation, operation-specific status
flags and the accumulator-overflow latch. AnyPS2 extends its original integer
significand arithmetic and LLVM lowering; no upstream implementation is copied.
The scalar policy is separate from the existing bounded VU helpers. This is
reference-informed behavior, not physical-hardware validation; the documented
multiplier partial-product precision limitation remains.

### Vector extended-range arithmetic

PSI-Rockin's public [ps2tek](https://github.com/PSI-Rockin/ps2tek/tree/1c9166066c3ab9ad089a4d12088acaa9476df4b6)
explains the finite exponent-255 format and VU MAC/status lane layout. The pinned
PCSX2 `VUflags.cpp` and `VUops.cpp` cross-check flag ownership, masked lanes and
separate multiply/add stages. AnyPS2 reuses its original integer-significand
arithmetic for both compiled VU regions and EE vector instructions. No reference
implementation is copied or linked. MAC overflow includes exponent-255 results;
status keeps sticky flags and the independent quotient flags.

Multiply-accumulate supports extended operands and unambiguous saturation when
an overflowing product is added to zero or a same-sign accumulator. Opposing-sign
accumulation after product overflow and intermediate product underflow remain
explicit unsupported boundaries pending validation of the internal VU pipeline.
This extension does not claim bit-exact multiplier partial-product precision.

### Highlight texture functions

The pinned PCSX2 Vulkan `bin/resources/shaders/vulkan/tfx.glsl` `tfx` equations
were consulted for HIGHLIGHT/HIGHLIGHT2 RGB modulation plus vertex alpha, their
distinct alpha equations, TCC selection and channel saturation. AnyPS2 extends
its own integer texture-combine shader for sprites and triangles; no upstream
shader code is copied. Original packet fixtures verify GPU readback for all four
functions, RGB/RGBA selection and saturation.

### Native Vulkan attachment path

The Khronos Vulkan specification, Render Pass chapter and Synchronization
Examples (consulted 2026-11-03), define same-subpass input/colour attachment
feedback and framebuffer-local dependencies. The implementation is original;
no sample implementation is copied. Integer colour/depth attachments retain
observable integer operations while Vulkan performs primitive rasterization.

- https://docs.vulkan.org/spec/latest/chapters/renderpass.html
- https://docs.vulkan.org/guide/latest/synchronization_examples.html

The Khronos [Swapchain Semaphore Reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
(consulted 2026-11-03) defines the semaphore lifetime used by native presentation:
acquire semaphores belong to bounded in-flight frames and retire through graphics
fences; presentation wait semaphores belong to swapchain images and are reused
after image reacquisition. This is an original implementation of the documented
API contract, with no sample code copied.

The Khronos [Vulkan fragment interlock specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html)
and [GLSL ARB fragment interlock revision 2](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_shader_interlock.txt)
(consulted 2026-11-03) define ordered per-pixel critical sections and coherent
image access. AnyPS2 independently implements programmable integer colour/depth
operations using these guarantees, selected by device capability rather than
GPU vendor or game identity. No specification sample implementation is copied.

### Native vector arithmetic acceleration

The [Intel C++ intrinsics reference](https://www.intel.com/content/dam/develop/external/us/en/documents/cpp_compiler_classic.pdf)
was consulted for AVX-512 embedded rounding and exception suppression. The native
vector fast path uses separate round-toward-zero operations only after proving
that the selected operands/results stay in the supported normal/zero range.
CPU/OS feature checks gate these instructions; extended exponents, underflow and
other exceptional cases retain the existing integer implementation. No reference
implementation is copied, and no fused multiply-add substitution is enabled.

### Antialiased triangle interiors

The pinned PCSX2 revision `1f67c53b6723fcd6a90041a5b296f0e34a6a3222`,
`bin/resources/shaders/vulkan/tfx.glsl` and
`pcsx2/GS/Renderers/SW/GSDrawScanline.cpp`, distinguishes fully opaque triangle
interiors from antialiased exterior edges and suppresses depth updates for the
exterior. These were consulted as behavioral references; no implementation was
copied. AnyPS2's independent regression uses original synthetic triangles to
verify that interior pixels remain opaque and occlude a farther surface. Its
existing sampled exterior coverage remains an approximation.

### LLVM Vulkan kernel compilation

- Reference: [LLVM SPIR-V target guide](https://llvm.org/docs/SPIRVUsage.html),
  with the intrinsic ABI checked against LLVM `llvmorg-22.1.8`, commit
  `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`.
- Public interface examples: `llvm/test/CodeGen/SPIRV/hlsl-resources/StructuredBuffer.ll`
  and `llvm/test/CodeGen/SPIRV/hlsl-intrinsics/SV_DispatchThreadID.ll` in that revision.
- LLVM is Apache-2.0 WITH LLVM-exception. Local toolchain sources remain ignored;
  no LLVM implementation is copied into AnyPS2.
- AnyPS2's arithmetic graph, emitter and test data are original. CPU and GPU
  arithmetic reuse the same AnyPS2 finite-arithmetic implementation. Generated
  LLVM, linked bitcode and validated SPIR-V are build artifacts. The arithmetic
  kernel interface alone does not establish EE/VU region legality or game use.

## PS2SDK rendering samples

The Teapot integration input is `ee/draw/samples/teapot` from the PS2SDK revision
`2a18d15da1332665451a9bcf9a0d4df9029c9490` recorded above (AFL-2.0 and file notices).
Its source, mesh and converted artifacts remain ignored. Both native and PCSX2
comparison use the same locally built ELF. The local public `graph.c` helper
acknowledges VSYNC/HSYNC with their write-one-to-clear masks instead of read/OR/write,
so acknowledging one event does not also clear FINISH. Teapot drawing code is unchanged.
The `ee/draw/samples/vu1` sample at the same pin exercises packet2 and a separately
assembled VU1 program. Its source, texture, microprogram and converted artifacts
also stay ignored; the local build uses the same SDK synchronization correction.
The `ee/libgs/samples/libgs_doublebuffer` and `ee/libgs/samples/libgs_draw` inputs use unmodified demo source at
the same pin. The `ee/draw/samples/texture` input uses the synchronization
correction above and embeds its original `texture.raw` locally, as its upstream
build does. These sample sources, assets and generated artifacts remain ignored.
The pinned PCSX2 `GSDeviceVK::DoMerge` and `merge.glsl` were consulted for the
RC1 constant-alpha display blend against BGCOLOR when RC2 is disabled. The native
composition change and synthetic colour fixtures are original implementations.

## gsKit integration samples

Point rounding and non-antialiased line coverage were checked against
`GS/Renderers/SW/GSRasterizer.cpp` at the PCSX2 revision pinned above.
AnyPS2 independently evaluates the endpoint diamond-exit rule and rational
interpolation in Vulkan shaders; no rasterizer implementation is copied.
The same pin's `GSState::IsCoverageAlpha` limits AA1 coverage to
lines and triangles; point and sprite draws retain ordinary coverage and alpha.

The local graphics examples use [ps2dev/gsKit](https://github.com/ps2dev/gsKit)
at `8ef73d05f022d14b3ded7c70cba670fe333ed61a` (AFL-2.0 and per-file notices),
with the gsKit/dmaKit archives supplied by the PS2DEV v2.0.0 toolchain.
The upstream checkout, sample assets, PS2 ELFs and converted output stay ignored.

AOT-recognized VSYNC polls use native display deadlines and bounded host waits.
The Windows wait follows Microsoft's public [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
and [SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer)
contracts. It uses an unnamed, thread-owned high-resolution timer, with standard
host sleep if unavailable; no timer sample code is copied.

## Tyra integration input

The local full-demo input uses [Tyra](https://github.com/h4570/tyra) at
`b4be27a2f6f9bde4f3240cb3f3bfb70f18c1e050` (Apache-2.0 source notices).
Its PS2 build uses the public PS2DEV toolchain above,
[OpenVCL](https://github.com/ps2dev/openvcl) at
`a5e9dbee6c0f2033403ee846fcc8565f17a0840d` (AFL-2.0), and
[vclpp](https://github.com/glampert/vclpp) at
`95911cd3f6842978b403b6f3a9d1dbcdfe21f09e` (MIT).
The local build supplies a missing `<functional>` include and excludes the
standalone banner-data object already included by its caller. It does not use
the bundled proprietary VCL executable. Upstream assets have separate provenance;
the release asset archive, source checkouts, PS2 ELF and conversion output remain
ignored and are not distributed with AnyPS2. Preparation is not compatibility proof.

The pinned Play! `PS2OS.cpp` SetupHeap/EndOfHeap interface behavior informs the
native startup boundary service: size `-1` selects the main stack base regardless
of the supplied heap start; an explicit size selects start plus size. This records
a boundary rather than allocating a heap. Native memory bounds remain checked.

The pinned PCSX2 `SPR.cpp` and `Dmac.cpp` were consulted for scratchpad transfer
direction, 16 KiB wrap, descriptor ordering and full-quadword tag transfer. The
native implementation uses independently written validated buffer-copy batches;
it does not copy the emulator's channel scheduling or timing implementation.
