# Project history and verified milestones

AnyPS2 began as SamG-Coder's experiment in converting user-supplied PS2 programs
into native PC software, with Gran Turismo 4 as the first commercial target.
Implementation and review have used Codex and, during part of development, Grok.
AI assistance does not establish correctness or provenance.

Early diagnostic hardware models helped investigate startup. The production goal
became LLVM ahead-of-time compilation with native C++ services and Vulkan rendering.
Existing register-state and address adapters remain migration work toward recovered
native functions and module bindings. Runtime interpretation and JIT are not
production fallbacks.

Original cross-compiled PS2 tests exposed callback and object-layout failures that
simple instruction fixtures missed. Public homebrew programs now exercise a broader
rendering, input and audio path. Historical changes remain in Git rather than being
duplicated here as a per-operation log.

## Working homebrew demos

| Locally adapted program | Verified behavior | Limits |
| --- | --- | --- |
| glampert Cubes | Animated textured scene; user confirmed DualSense camera control | Original inverted vertical look retained |
| glampert Console/Input | User confirmed button labels and vibration start/stop | Supported native gamepad capabilities only |
| glampert Audio/Sound | Native XAudio2 sample playback; user reported it sounds about the same as PCSX2 | Listening comparison, not waveform equivalence or general game audio |
| glampert Dungeon | User confirmed solid armour and smooth movement after rendering fixes | Rotation slowdowns remain a performance concern; no complete-game claim |
| PS2SDK Teapot | Four rotating lit teapots in native Vulkan; a 20-second run measured about 59–60 display updates/s | Narrow sample coverage; not a benchmark against PCSX2 |
| PS2SDK VU1 | Textured cubes rendered through separately AOT-compiled EE and VU1 code | Visual capture and bounded run; full reference parity not established |
| PS2SDK Double-buffer | Moving coloured triangles with alternating buffers | Native line-doubling differs from PCSX2 deinterlacing; hardware parity unverified |
| PS2SDK Texture | Rotating flower-textured cube at 640×512; bounded native run and capture | Reference and hardware parity not established |
| PS2SDK Sprites | 1,000 moving sprites; user confirmed appearance after native framebuffer mapping and display-alpha fixes | Visual PCSX2 comparison; hardware parity unverified |
| PS2SDK Cube | Rotating coloured cube; user confirmed it matches PCSX2 | Visual comparison; hardware parity unverified |
| gsKit Basic | Points, lines, triangle strips/fans, clipping and blending; user confirmed it matches PCSX2 | Native VSYNC waits verified; hardware parity unverified |

These are locally adapted inputs, not unmodified-upstream compatibility claims.
Ports include compiler compatibility, native service binding and a public SDK
write-one-to-clear synchronization correction. The demo source, assets, PS2 ELFs,
converted output and captures remain outside Git. Upstream source licenses do not
necessarily cover every included asset. See [source references](source-references.md)
and [homebrew testing](ps2-test-programs.md#larger-homebrew-tests).

## Commercial and provenance boundaries

GT4 has displayed its opening warning through Vulkan. Its title screen and playable
gameplay have not been demonstrated. Homebrew progress does not establish GT4 support.

Final Fantasy XII (PAL) reached its title screen, opening movie and first real-time 3D
scene in headless scripted runs, with EE, IOP and VU code compiled ahead of time and no stop
in seven minutes. Character control, later scenes, frame rate and visual or audio accuracy
have not been established. No memory card is exposed, so saving is unavailable; the game
continues after its own prompt for that. The menu overlay compiled into its executable stops
if called.

Public references include PCSX2, PS2SDK, Play! and recompilation projects. Their
roles, pins and licenses are recorded in the [provenance ledger](source-references.md).
PCSX2 is used only as an external reference when requested. No games, BIOS/firmware,
proprietary SDKs or converted commercial binaries are distributed. Repository-content
checks and human provenance review are required before publication.
