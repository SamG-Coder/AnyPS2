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

Discovery is bounded and conservative. Executable-segment membership alone does
not establish that bytes are instructions: tables, constants and VU payloads can
occupy those segments. Unresolved callbacks remain coverage gaps. See
[EE conversion](ee-native.md) and [flow verification](flow-verification.md).

## Native services

- Memory services preserve guest widths, aliases, initialization and writable data.
  Writes into compiled instruction ranges must stop rather than silently modify code.
- Scheduling and synchronization preserve observable wait, callback and completion
  order. Host elapsed time is distinct from console instruction counts.
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
  thresholds. Whole-picture IDEC, RGB16 conversion and PACK remain unsupported.
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
Asynchronous VU1 submissions publish ordered graphics from completed launches
while later compiled launches continue. A later failure stops execution and does
not report submission completion; already accepted graphics remain submitted.
A native drawing worker accepts one bounded, owned packet batch at a time.
The execution owner rejoins it before reading graphics state or presenting output.
Worker failures surface through the native service boundary; recording a batch
does not itself signal FINISH, which still follows the actual GPU fences.
FINISH and display operations track actual submitted work. Presentation resources
can be rebuilt when the display dimensions change without discarding game surfaces.
The window reports rendered display updates; presentation frequency alone is not
simulation speed or proof that every displayed image contains new content.

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
