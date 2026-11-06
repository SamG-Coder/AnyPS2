# Source references and provenance

This ledger records dependencies, behavioral references and separately acquired
integration inputs. It is not a change log or a compatibility report. Record a
reference once, with its revision, purpose, license and any adaptation; keep
operation-by-operation history in Git. See [licensing](licensing.md) for distribution
requirements and [architecture](architecture.md) for current runtime boundaries.

No proprietary BIOS, SDK, game instructions or assets are incorporated here.
Unless explicitly identified as a dependency below, references informed original
implementations and are not copied or linked into AnyPS2. Consulting a GPL project
does not permit incorporating its implementation under AnyPS2's MIT license.

## Linked dependencies

| Dependency | Pin | Use and retained notice |
| --- | --- | --- |
| zlib 1.3.1 | Archive SHA256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` | Unmodified conversion-time decompression; [zlib notice](third-party/zlib-LICENSE.txt) |
| HIDAPI 0.15.0 | `dbff4ea89f55a572aeb0c53a7b32ea70853ec260` | Native USB report descriptions; BSD license option, [notice](third-party/hidapi-license.txt) |
| SDL | `bd9f6ca9ff0e9509f49980fc1b70837f54b6f244` | Static input/gamepad/haptics provider; [zlib-style notice](third-party/SDL-license.txt) |

SDL archive SHA256: `6d820fec9af8b0a9d1fe386dd9c41a401df0a564ea941570140391da765b36c3`.
Build definitions own dependency selection. Local game binaries and reference
applications are never bundled with these dependencies.

## Console behavior and file formats

| Reference | Reviewed revision | Purpose and provenance |
| --- | --- | --- |
| PCSX2 | `1f67c53b6723fcd6a90041a5b296f0e34a6a3222` | GPL-licensed behavioral reference for EE/IOP/VU instructions, public device interfaces and GS format/raster rules; no source linked or imported |
| PS2SDK | `2a18d15da1332665451a9bcf9a0d4df9029c9490` | Public syscall/import ordinals, structures, RPC and module contracts; applicable file notices and AFL-2.0 remain upstream. audsrv implementation is LGPL-2.0 and is only an interface reference |
| Play! | `1f27232772e484bfb79364b751b84f41f4c87481` | EE startup, thread/interrupt, module, file and sysclib behavior; retained [BSD attribution](third-party/Play-license.txt) |
| Play! Framework | `587f278917acc0026bf5fc34b39f995fc26bd015` | MPEG coefficient, DC-size, coded-block-pattern, scan and quantizer tables adapted in `mpeg_tables.hpp`; BSD terms retained in that file and the [Play! notice](third-party/Play-license.txt). No framework IDCT implementation imported |
| ps2tek | `1c9166066c3ab9ad089a4d12088acaa9476df4b6` | Public vector numeric, MAC/status and SIO2 interface documentation |
| ARMSX2 | `05f55b8e197b6a5b31d2b258a3827a6b4769478b` | Published VU0 hardware-capture behavior; GPL-3.0-or-later, copyright 2026 ARMSX2 Dev Team. No implementation or test tables copied |
| PS2DEV binutils | `3eb45ea37f0efd498d1de3cf9562de07197aefa8` | DVP overlay metadata: `include/elf/mips.h` (GPL-3.0-or-later), `gas/config/tc-dvp.c` (GPL-2.0-or-later); independent reader, no assembler copied |
| ps2rd | `b433fa34bed0edb2665d5d6bd4f45585606aed74` | Public EE syscall documentation; interface reference only |
| Gran Turismo Modding Hub | `d70af9d3db86aa1fb0f91ff7ce7ee8ad719b4c61` | Public segmented raw-deflate executable-container fields; independent importer, no keys or game routines |

The principal source areas consulted at those pins are:

- **EE and memory:** PCSX2 `R5900OpcodeTables.cpp`, `R5900OpcodeImpl.cpp`, `MMI.cpp`,
  `FPU.cpp`, `COP0.cpp`, `R5900.cpp` (exception boundaries), `Cache.cpp`, `x86/iFPUd.cpp`; Play! `Source/ee/PS2OS.cpp`
  and `Source/ee/Vif.cpp` (reset CYCLE value);
  PS2SDK `ee/startup`, `ee/kernel` (including Cause, BadVAddr and BadPAddr
  selector names in `kernel.h`) and public ELF section/symbol specifications.
- **IOP and native services:** PS2SDK `iop/system`, `iop/kernel`, `iop/fs`, `iop/cdvd`,
  `iop/network`, `iop/usb`, `iop/input`, `iop/sound`, `iop/memorycard` (public `MCMAN`
  and `MCSERV` call behaviour, result codes and entry layout), `ee/rpc`, plus public
  `libsd-common.h`, `fileio-common.h`, `sifcmd-common.h` and `sifrpc-common.h`;
  PS2SDK `threadman/src/thbase.c` and `thcommon.c` for thread-entry stack ABI;
  Play! `Source/iop/IopBios.cpp`, `Iop_FileIo*` and `Iop_Sysclib*`.
- **Transfers and vector work:** PCSX2 `Sif0.cpp`, `Sif1.cpp`, `Dmac.cpp`, `Hw*.cpp`,
  `SPR.cpp`, `Gif_Unit.h`, `Vif*.cpp`, `Vif.h`, `VUops.cpp`, `VUflags.cpp`,
  `x86/microVU_Lower.inl`, `microVU_Analyze.inl`, `microVU_Compile.inl` and
  `microVU_Misc.h`; PS2SDK packet2 and public VIF/upload interfaces.
- **Graphics:** PCSX2 `GS.h`, `GS.cpp`, `GSRegs.h`, `GSState.cpp`, `GSTables.cpp`,
  `GSClut.cpp`, `GSLocalMemory.h`, `GSRendererSW.cpp`, `GSRasterizer.cpp`,
  `GSDrawScanline.cpp`, `GSRendererHW.cpp` (16-bit write-mask mapping),
  `GSDeviceVK::DoMerge`, Vulkan `tfx.glsl` and `merge.glsl`;
  `Counters.cpp`, `MTGS.cpp` and `GSRenderer.cpp` for field/display ordering.
  The image importer independently implements PSMCT16/PSMCT16S address-bit
  permutations checked against the public GS format tables. Palette conversion
  preserves CSM1/CSM2 selection, shared 16/32-bit entry storage and draw-time TEXA expansion.
  TEX1 LOD level rounding, MIPTBP level addressing and per-level clamp scaling follow
  `GSRendererSW.cpp`/`GSDrawScanline.cpp` behavior; no PCSX2 source is imported.
- **Video initialization:** PCSX2 `IPU/IPU.cpp`, `IPU/IPU.h`, `IPU/IPUdma.cpp`
  and `IPU/IPU_MultiISA.cpp` for reset,
  bitstream-position clearing, format/threshold settings, table upload, header
  reads, intra-slice sequencing and completion events;
  independent native command implementation, no PCSX2 decoder source imported.
  Native MPEG header VLC tables in `ee_video_vlc.hpp` are adapted from Play!
  `Source/ee/IPU_*Table.cpp` at the pin above, under its retained BSD notice.
  These media-format tables decode video data, not executable instructions.
  `IPU/yuv2rgb.cpp` documents the BT.601 coefficient precision and rounding used
  as the color-conversion contract; no PCSX2 conversion implementation is imported.
- **Input and audio:** PS2SDK PADMAN `sio2Cmds.c`, `padCmds.c`, `libpad.c`, audsrv,
  `iopheap.c`, `tools/adpenc/src/adpcm.c`, LIBSD `voice.c` and `block.c`;
  PCSX2 `SIO/Sio2.cpp`, `SIO/SioTypes.h`, `SIO/Pad/PadDualshock2.cpp` (command negotiation)
  and `SPU2/Mixer.cpp`;
  public psx-spx controller and USB HID specifications.

Native services replace supported operations with explicit host ownership and
completion. These interface references do not authorize BIOS reconstruction or
success stubs. Native source material and all synthetic fixtures are independently
written. A user-owned BIOS may be used with an external reference emulator; it is
not an AnyPS2 input or output dependency. PCSX2 v2.8.2 and its public PINE protocol
were used separately for local comparisons, never as a production fallback.

Important interpretation limits:

- VU0 capture results inform shared FMAC behavior; this is not independent VU1
  hardware validation. Multiplier partial-product precision remains a limitation.
- Native antialiased exterior coverage is approximate. Progressive reconstruction
  of interlaced fields is a display policy, not a CRT simulation.
- Windows HIDAPI reconstructs an equivalent report descriptor; it does not recover
  the original wire encoding. Native audio resampling is not console interpolation.
- External examples and emulator observations are evidence about specific behavior,
  not proof of complete hardware equivalence or commercial-game compatibility.

## Recompilation research

| Project | Revision | Use and license boundary |
| --- | --- | --- |
| moesuito/gt4-staticrecomp | `795cdc086464b003d49c0b44d0911dc761f58f5c` | Architecture, IOP map and third-party notes. Reports MIT for original work; dependencies have separate terms. No implementation imported |
| snuri00/ps2-web-recomp | `1b4808793a12c45125262fd2d5f50a9ec9f7c020` | SIF and I/O service organization; MIT as recorded by the reference. No implementation imported |
| KIexster/god-of-war-recomp | `2d3c91dc173727058073c267bbfb27b3dab703a1` | VU generator, block/delay hazards and sticky-flag patches; GPL-3.0. No implementation imported |

These projects' generated C++, game overrides, interpreters and reference renderers
are not AnyPS2 production paths. Reported upstream gameplay has not been validated
as AnyPS2 compatibility.

## Host APIs and compiler interfaces

- **Vulkan/GLSL:** Khronos render-pass, synchronization, copy/update, WSI, semaphore
  reuse, mailbox and fragment-interlock contracts informed original graphics and
  presentation implementations. GLSL ARB fragment interlock revision 2; API pages
  consulted 2026-11-01 through 2026-11-03. No sample shader or renderer copied.
- **LLVM:** `llvmorg-22.1.8`, `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`,
  Apache-2.0 WITH LLVM-exception. The SPIR-V target guide and public
  `StructuredBuffer.ll` / `SV_DispatchThreadID.ll` examples define the experimental
  kernel ABI. AnyPS2's graph/emitter is original; kernel compilation alone does
  not establish legality of converting an arbitrary EE/VU region to GPU execution.
- **Intel:** published intrinsic contracts for AVX-512 rounding and exception
  suppression inform guarded native arithmetic. Exceptional values retain the
  existing numeric implementation; no reference implementation copied.
- **Microsoft:** Windows SDK 10.0.28000.0 and public ETW/PMC, timing, USB/SetupAPI,
  HID and XAudio2 contracts inform native providers. Independent bounded parsers
  and resource owners; no SDK/profiler/mixer implementation copied.
- **PerfView:** `9f071a5cb2e70cdfee13abc510344ab1e8840c90`, MIT, copyright .NET
  Foundation and Contributors; `CSwitchTraceData` context-switch record fields
  only, not parsing implementation. Unsupported layouts fail explicitly.
- **ELF/GCC:** public section/symbol specifications and GCC 3.0.4 initialization
  framing inform original image and constructor discovery; no runtime copied.

## Development tools and integration inputs

These are separately acquired and kept under ignored directories. They are not
shipped dependencies or permission to redistribute assets.

| Input/tool | Revision | License / local adaptation |
| --- | --- | --- |
| PS2DEV v2.0.0 | `b1eb649a3765bf68500cfba9bb62433c526c90a8` | Public cross-toolchain; GCC/binutils and SDKs retain their own licenses. Original synthetic C++/assembly fixtures do not link the SDK |
| glampert/ps2-homebrew | `602441a6877a3136709d6320664340d52e3027a1` | MIT source, Guilherme R. Lampert 2015; assets require separate review. Local compiler compatibility and native service/input bindings |
| PS2SDK samples | PS2SDK pin above | Teapot, VU1, texture, libgs double-buffer/draw and other inputs; AFL-2.0 and per-file notices. Original sample assets remain ignored |
| gsKit | `8ef73d05f022d14b3ded7c70cba670fe333ed61a` | AFL-2.0 and per-file notices; gsKit/dmaKit archives from PS2DEV |
| Tyra | `b4be27a2f6f9bde4f3240cb3f3bfb70f18c1e050` | Apache-2.0 source; separate asset provenance. Local missing `<functional>` include and duplicate banner-object correction |
| OpenVCL | `a5e9dbee6c0f2033403ee846fcc8565f17a0840d` | AFL-2.0; public Tyra build tool, not the bundled proprietary VCL executable |
| vclpp | `95911cd3f6842978b403b6f3a9d1dbcdfe21f09e` | MIT; public build preprocessor |

PS2DEV Windows archive `ps2dev-windows-latest.tar.gz` SHA256:
`bab98ee210c62aa3bd98d9bdda2af3f92a628e6e3d63e4b52f243ae56e3eb4ce`.
MSYS2 mingw32 support packages were checked against official repository SHA256
records; package manifests/notices stay with the ignored installation.

Local PS2SDK `graph.c` acknowledges VSYNC/HSYNC using write-one-to-clear masks
rather than read/OR/write, avoiding accidental FINISH acknowledgement. Relevant
homebrew/PS2SDK builds use this explicit SDK adaptation. The controller-enabled
homebrew port binds supported native RPC endpoints rather than loading ROM modules.
These are adapted integration inputs, not claims of unmodified-upstream support.
The local audsrv example links its public EE client with upstream notices retained.
No upstream implementation, demo source, texture, mesh, microprogram, converted
binary or capture is incorporated into the repository. See [verified results](project-history.md).

## Evaluated library candidates

These pins record research, not enabled dependencies. Reassess ABI, ownership,
completion, licensing and notices before integrating a candidate.

| Candidate | Pin | License / considered purpose |
| --- | --- | --- |
| Asio | `bdf337ddb1a667c274372a602de5857ad3cb3ed0` | Boost Software License 1.0; networking |
| libusb | `987c1170d0018ba82557823a98ae443e8335e11b` | LGPL-2.1-or-later; USB transfers |
| miniaudio | `877ea4a7c2601c7b0f2a44e83dfac42e6470fea6` | MIT-0 or public domain; PCM output/mixing |
| Vulkan Memory Allocator | `0f1a757da579831283413295cc7f3b47eb521c77` | MIT; GPU allocations |

## Reference links

The following source/API links support the entries above. Repository revisions are
pinned in the tables; live documentation URLs do not imply a newer source pin.

### Vulkan and graphics APIs

- [swapchain_semaphore_reuse.html](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
- [synchronization_examples.html](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
- [VK_EXT_fragment_shader_interlock.html](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html)
- [VkPresentModeKHR.html](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html)
- [vkCmdCopyBuffer.html](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer.html)
- [vkCmdUpdateBuffer.html](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdUpdateBuffer.html)
- [wsi.html](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)
- [renderpass.html](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
- [synchronization.html](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
- [ARB_fragment_shader_interlock.txt](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_shader_interlock.txt)

### Other interface and tool references

- [03-sheader.html](https://gabi.xinuos.com/elf/03-sheader.html)
- [05-symtab.html](https://gabi.xinuos.com/elf/05-symtab.html)
- [mips_instruction_set_reference.pdf](https://gamingdoc.org/wp-content/uploads/2020/07/mips_instruction_set_reference.pdf)
- [Initialization.html](https://gcc.gnu.org/onlinedocs/gcc-3.0.4/gcc/Initialization.html)
- [libusb.info](https://libusb.info/)
- [group__libusb__asyncio.html](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html)
- [SPIRVUsage.html](https://llvm.org/docs/SPIRVUsage.html)
- [miniaud.io](https://miniaud.io/)
- [ps2_core](https://nenkai.github.io/gt-modding-hub/formats/ps2_core/)
- [group__packet2__types.html](https://ps2dev.github.io/ps2sdk/group__packet2__types.html)
- [ps2tek](https://psi-rockin.github.io/ps2tek/#sio2registers)
- [controllersandmemorycards](https://psx-spx.consoledev.net/controllersandmemorycards/)
- [mingw32](https://repo.msys2.org/mingw/mingw32/)
- [License](https://think-async.com/Asio/License)
- [networking.html](https://think-async.com/Asio/asio-1.36.0/doc/asio/overview/networking.html)
- [CategoryGamepad](https://wiki.libsdl.org/SDL3/CategoryGamepad)
- [FAQLicensing](https://wiki.libsdl.org/SDL3/FAQLicensing)
- [cpp_compiler_classic.pdf](https://www.intel.com/content/dam/develop/external/us/en/documents/cpp_compiler_classic.pdf)
- [page-2](https://www.psx-place.com/threads/newlib-porting-challenges.26821/page-2)
- [hid1_11.pdf](https://www.usb.org/sites/default/files/hid1_11.pdf)
- [zlib-1.3.1.tar.gz](https://zlib.net/fossils/zlib-1.3.1.tar.gz)

### Source repositories

- [ARMSX2/ARMSX2 — recompilers](https://github.com/ARMSX2/ARMSX2/tree/05f55b8e197b6a5b31d2b258a3827a6b4769478b/tests/ctest/core/recompilers)
- [GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
- [KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp)
- [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2)
- [PCSX2/pcsx2 — v2.8.2](https://github.com/PCSX2/pcsx2/releases/tag/v2.8.2)
- [PSI-Rockin/ps2tek](https://github.com/PSI-Rockin/ps2tek/tree/1c9166066c3ab9ad089a4d12088acaa9476df4b6)
- [glampert/ps2-homebrew](https://github.com/glampert/ps2-homebrew/tree/602441a6877a3136709d6320664340d52e3027a1)
- [glampert/vclpp](https://github.com/glampert/vclpp)
- [h4570/tyra](https://github.com/h4570/tyra)
- [jpd002/Play- — PS2OS.cpp](https://github.com/jpd002/Play-/blob/master/Source/ee/PS2OS.cpp)
- [libusb/hidapi](https://github.com/libusb/hidapi/tree/dbff4ea89f55a572aeb0c53a7b32ea70853ec260)
- [microsoft/perfview — LICENSE.TXT](https://github.com/microsoft/perfview/blob/9f071a5cb2e70cdfee13abc510344ab1e8840c90/LICENSE.TXT)
- [microsoft/perfview — KernelTraceEventParser.cs](https://github.com/microsoft/perfview/blob/9f071a5cb2e70cdfee13abc510344ab1e8840c90/src/TraceEvent/Parsers/KernelTraceEventParser.cs)
- [mlafeldt/ps2rd — ee-syscalls.txt](https://github.com/mlafeldt/ps2rd/blob/b433fa34bed0edb2665d5d6bd4f45585606aed74/Documentation/technical/ee-syscalls.txt)
- [moesuito/gt4-staticrecomp](https://github.com/moesuito/gt4-staticrecomp)
- [ps2dev/binutils-gdb](https://github.com/ps2dev/binutils-gdb/tree/3eb45ea37f0efd498d1de3cf9562de07197aefa8)
- [ps2dev/gsKit](https://github.com/ps2dev/gsKit)
- [ps2dev/openvcl](https://github.com/ps2dev/openvcl)
- [ps2dev/ps2dev](https://github.com/ps2dev/ps2dev/tree/b1eb649a3765bf68500cfba9bb62433c526c90a8)
- [ps2dev/ps2sdk — iopheap.c](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/ee/kernel/src/iopheap.c)
- [ps2dev/ps2sdk — audsrv_rpc.c](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/ee/rpc/audsrv/src/audsrv_rpc.c)
- [ps2dev/ps2sdk — eefixconf.c](https://github.com/ps2dev/ps2sdk/blob/2a18d15da1332665451a9bcf9a0d4df9029c9490/tools/srxfixup/src/eefixconf.c)
- [ps2dev/ps2sdk — kernel.h](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h)
- [ps2dev/ps2sdk — syscallnr.h](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/syscallnr.h)
- [ps2dev/ps2sdk — crt0.c](https://github.com/ps2dev/ps2sdk/blob/master/ee/startup/src/crt0.c)
- [ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk/tree/2a18d15da1332665451a9bcf9a0d4df9029c9490)
- [snuri00/ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp)

### Windows APIs

- [ni-usbioctl-ioctl_usb_get_descriptor_from_node_connection](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_descriptor_from_node_connection)
- [ni-usbioctl-ioctl_usb_get_node_connection_information_ex](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_node_connection_information_ex)
- [ns-usbioctl-_usb_descriptor_request](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-_usb_descriptor_request)
- [ns-usbioctl-_usb_node_connection_driverkey_name](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-_usb_node_connection_driverkey_name)
- [hid-architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture)
- [recording-pmu-events](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events)
- [ns-evntcons-event_header_extended_data_item](https://learn.microsoft.com/en-us/windows/win32/api/evntcons/ns-evntcons-event_header_extended_data_item)
- [ne-evntrace-trace_query_info_class](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ne-evntrace-trace_query_info_class)
- [nf-realtimeapiset-querythreadcycletime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
- [nf-synchapi-createwaitabletimerexw](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [nf-synchapi-setwaitabletimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer)
- [nf-winbase-enablethreadprofiling](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-enablethreadprofiling)
- [nf-winbase-readthreadprofilingdata](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readthreadprofilingdata)
- [nf-xaudio2-ixaudio2-createmasteringvoice](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2-createmasteringvoice)
- [nf-xaudio2-ixaudio2sourcevoice-getstate](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-getstate)
- [XAudio2 processing callbacks](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-callbacks)
- [nf-xaudio2-ixaudio2sourcevoice-submitsourcebuffer](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-submitsourcebuffer)
- [nf-xaudio2-ixaudio2voice-seteffectchain](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-seteffectchain)
- [nf-xaudio2-ixaudio2voice-setoutputmatrix](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-setoutputmatrix)
- [nf-xaudio2fx-reverbconverti3dl2tonative](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-reverbconverti3dl2tonative)
- [nf-xaudio2fx-xaudio2createreverb](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-xaudio2createreverb)
- [configuring-and-starting-a-systemtraceprovider-session](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session)
- [cswitch](https://learn.microsoft.com/en-us/windows/win32/etw/cswitch)
- [thread-v2](https://learn.microsoft.com/en-us/windows/win32/etw/thread-v2)
- [how-to--initialize-xaudio2](https://learn.microsoft.com/en-us/windows/win32/xaudio2/how-to--initialize-xaudio2)
- [xaudio2-volume-and-pitch-control](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-volume-and-pitch-control)
