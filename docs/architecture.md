# Architecture

AnyPS2 converts discovered PS2 code ahead of time through LLVM and links native
C++20 services. Runtime instruction interpretation, JIT compilation and emulator
fallback are not production paths. Unsupported behavior stops with diagnostics.

## Conversion pipeline

1. Import ISO9660 files and identify boot executables, IRX modules and known containers.
2. Parse image sections, relocations, exports and executable ranges.
3. Discover control flow and callbacks; keep ambiguous data references separate.
4. Lower supported EE, IOP and VU operations and emit inspectable LLVM artifacts.
5. Compile native objects and link reusable services into a Windows x64 executable.

The inventory records discovery, analysis, generation and compilation separately.
A generated object is not proof that its module links, executes or initializes.
Artifact paths encode source identities deterministically, including case-only
name differences. See [batch conversion](batch-conversion.md) for commands and reports.

## Code and module boundaries

Programs own CLI handling; libraries own formats, decoding, analysis, generation,
services and rendering. The IOP typed IR records operations and operands rather
than source strings. It is still a low-level representation; EE and VU do not yet
share a complete high-level function IR.

Known module calls should resolve through native symbols, relocations and explicit
exports/imports. Indirect calls use the actual runtime pointer and require a compiled
binding. Existing register-state and original-address adapters are migration work,
not the final native ABI. Native execution is not evidence that this migration is complete.

The native VU working state aligns vector lanes, the accumulator and data rows
to 16 bytes. Generated LLVM derives offsets and access alignment from that C++
layout. Artifact export version 3 requires regenerating older VU modules before
linking; native layout changes do not alter program-visible PS2 addresses.

Converted VU blocks are transactions that publish state only on success. A
block containing vector arithmetic is emitted twice. The fast block evaluates
lanes four at a time with host doubles, which give the exact chopped result for
operands whose result stays in the normal range. The cold exact block uses the
integer implementations. If any lane leaves that range, or the fast block
reaches a failure exit, the exact block's result is returned instead; results
and flags are identical either way. The MAC flag queue is tracked at conversion
time and written to VU state once per block.

Discovery is bounded and conservative. Executable-segment membership alone does
not establish that bytes are instructions: tables, constants and VU payloads can
occupy those segments. Unresolved callbacks remain coverage gaps. See
[EE conversion](ee-native.md) and [flow verification](flow-verification.md).

## Native services

- Memory services preserve guest widths, aliases, initialization and writable data.
  Writes into compiled instruction ranges must stop rather than silently modify code.
  Copied-code regions retain publication checks on each compiled straight-line
  path, including delay slots; unrelated writable state is excluded from that check.
- Scheduling and synchronization preserve observable wait, callback and completion
  order. Host elapsed time is distinct from console instruction counts.
  Display-line timers use the same nominal NTSC/PAL host-time schedule as scanout,
  independent of presentation throughput; this is not cycle-accurate video timing.
- EE/IOP communication connects compiled modules with native service providers.
  Registration, initialization, pending requests and completion are distinct states;
  transport completion is not proof that a caller has consumed a reply.
- Disc access is read-only and bounded. Guest paths must not escape to arbitrary
  host files. The [legacy FILEIO contract](fileio.md) describes that endpoint;
  other native file/module services have separate support boundaries.
- Video initialization supports reset, bitstream clearing, format/threshold settings
  and uploaded quantization/palette tables. Bounded native input submissions feed
  fixed-length and MPEG variable-length header reads and signal completion as
  their bytes are accepted. BDEC decodes six MPEG coefficient blocks, applies
  inverse quantization and an integer Q20 inverse DCT, and delivers planar 16-bit
  samples to a checked RAM/scratchpad destination. Input, output and command
  completion are separate. Queued input excludes the decoder's bounded look-ahead,
  so callers can refill partial symbols without losing retained bits.
  The IDCT is checked against the mathematical transform
  within one sample; bit-identical hardware rounding is not established. CSC converts
  planar 8-bit YCbCr batches to RGBA32 with defined coefficient rounding and alpha
  thresholds. IDEC reuses these stages for intra slices, retaining DC predictors
  between macroblocks and supporting field layout and signed RGB output. It waits
  for output consumption before decoding the next block and completes at the
  following start code. RGB16 conversion and PACK remain unsupported.
- Input uses SDL gamepad capabilities and haptics. Audio uses native XAudio2 owners
  for supported sample services. Open-ended ADPCM streams decode into bounded
  owned PCM queues; the host playback cursor drives refill and end notifications.
  Voice position reports the encoded word corresponding to consumed PCM, rather
  than modeling the console decoder's internal prefetch distance.
  Per-voice envelopes run on downstream 48 kHz native mixing stages. Zero pitch
  stops source consumption and holds its last resampled value while the envelope
  continues; source stop/resume and hold changes commit together in XAudio2.
  Stream refills preserve the current decoded block and revise unplayed PCM from
  saved predictor/loop checkpoints at a native processing-pass boundary. Failed
  revisions leave both encoded data and queued PCM unchanged. Revisions that
  shorten an already queued extent remain unsupported; they are never padded
  with silence. Unsupported device features are not advertised.

Diagnostic hardware models remain for investigation and synthetic regressions.
They are not evidence of native conversion and must not become runtime fallbacks.

## Vulkan rendering

Compiled EE/VU work submits supported graphics operations to native services.
The production raster path uses Vulkan images, graphics shaders and ordered batches.
It preserves supported depth, alpha, blending, texture, mask and submission-order
behavior; it is not a complete GS replacement. Reference rendering is a separate
validation path, not a fallback for an unsupported production draw.

Textures and render targets stay GPU resident where their dependencies permit.
Render-target dependencies track touched storage pages, not cached image capacity.
PSMCT16/PSMCT16S targets use Vulkan rasterization with packed write-mask and color
quantization semantics. Switching between 16- and 32-bit storage views currently
requires ordered host format conversion; color dithering remains unsupported
when it can affect written channels.
XYOFFSET changes smaller than one pixel on both axes keep the context's established
offset. Games cycle sub-pixel drawing offsets every frame so that an interlaced CRT
blends them into anti-aliased edges; a progressive display shows the whole picture
trembling instead. Placement therefore differs from the GS by less than one pixel;
a change of a pixel or more applies immediately. The presentation pass restores the
intended smoothing as a fixed vertical blend matching the held-back jitter range
(each neighbouring row weighted by a third of the jitter in display lines), so a
cycling ±¼-pixel offset in field-rendered output becomes a 1/6, 2/3, 1/6 blend.
It is applied while the game keeps jittering and dropped after about a second
without jitter. It is presentation only; captures and readbacks are unfiltered.
Z32 and Z24 frame and texture formats are drawn and sampled as CT32 and CT24 in
the depth page block order (block number XOR 24), the layout depth buffers use.
Z16 formats as colour targets or textures remain unsupported. PSMT8H, PSMT4HL and PSMT4HH textures take their palette index from bits 24-31, 24-27 or 28-31 of the CT32-layout word at each texel. A zero-width frame (FBW=0) places its pages side by side along x; for rows 0-31
its addresses equal those of a 2048-pixel-wide (FBW=32) frame, so frames scissored
to those rows draw with that width exactly. Below row 31 such a frame wraps row y onto row y%32; untextured sprites
reproduce that in 32-row bands drawn top to bottom, and other primitives on a
wrapping frame stop with a diagnostic.
TEX1 level-of-detail sampling binds MIPTBP levels 1-6 as separate native textures
and selects the magnification or minification filter per pixel. LOD is computed
in single-precision floating point from interpolated Q (or the sprite's Q), with
16 fractional bits for level selection and an 8-bit blend between adjacent levels;
it is not a bit-exact model of the GS LOD unit. Automatic base addresses (MTBA)
stop with a diagnostic when levels are sampled.
Asynchronous VU1 submissions form an ordered queue of up to 16 jobs run by one
worker thread; each starts from the program binding and VU state its predecessor
produced. The program can therefore submit later VIF1 packets, program uploads
and GIF (PATH3) transfers while earlier vector work runs, as a game hands work to
a render thread. VIF state, interrupts and graphics are published strictly in
submission order: VIF parsing continues from the state the last queued packet
leaves, and queued GIF or direct drawing reaches the renderer after the vector
output submitted before it. A PATH3 mask, interrupt stall or full queue still
waits. A failure stops execution, fails the jobs queued after it without running
them, and does not report their completion; already accepted graphics remain
submitted.
A native drawing worker records owned packet batches in order; up to 64 batches
and 64 MiB of packets may be queued, so bursts of small vector batches overlap
recording instead of stalling execution. Producers that own a batch hand it over
without a copy.
The execution owner rejoins it before reading graphics state. Window presentation
is queued on the worker after the frame's drawing, as on a game's render thread,
so execution continues while the frame is recorded and shown; the window thread
keeps resizing and title updates, and rejoins the worker before a resize.
Worker failures surface through the native service boundary; recording a batch
does not itself signal FINISH, which still follows the actual GPU fences.
FINISH and display operations track actual submitted work. Presentation resources
can be rebuilt when the display dimensions change without discarding game surfaces.
The window reports rendered display updates; presentation frequency alone is not
simulation speed or proof that every displayed image contains new content.
Half-height interlaced output is reconstructed progressively from the last three
composed images. A texel unchanged across them is shown as composed, and one that
repeats every third image, as sub-pixel drawing-offset jitter does on static
content, is shown as the mean over that cycle, as an interlaced display would
integrate it. Two-field alternation and motion use field reconstruction without
temporal trails. GS memory is unchanged; only presentation differs.

The experimental LLVM GPU texture path is disabled by default. The normal path
retains CPU texture preparation and native Vulkan rendering. Performance claims
require measured Release runs and a graphics-correctness check.

## Acceptance boundaries

Retain generated IR and build inputs locally so conversion is reproducible. Execute
generated code in relevant regressions and audit output dependencies for forbidden
runtime execution engines. A dependency audit is useful evidence, not a proof of
all semantics. Keep commercial inputs and generated artifacts outside Git.

Report generation, compilation, linking, execution, visible rendering and playable
interaction separately. Synthetic passes do not establish game compatibility.
See [testing](testing.md), [project history](project-history.md) and
[source references](source-references.md).
