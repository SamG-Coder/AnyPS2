# Native rendering modernization

This research branch targets recovered rendering computations and resources, not
a VU interpreter running on a GPU. It does not replace the working renderer yet.
The baseline is commit `2218a79be65ae1991842b5a402aea5b4fca61467`.

## Implemented boundary

`nt_render_codegen` lifts a contiguous, pure arithmetic VU region into the existing
`nt::gpu::Program` SSA graph. It reuses VU framing/decoding, removes dead arithmetic
and inputs, shares identical expressions, and emits ordinary float32 LLVM for a
native CPU function or a Vulkan compute kernel. Broadcasts, accumulator operations,
masked lanes, simultaneous source reads and vf0 constants are retained. Matrices
are not recognized by name: the synthetic transform reduces to its dataflow.

Inputs have explicit vertex or uniform rates. Uniform annotations are supplied by
the caller, not discovered or proven. This is an extraction experiment, not a
whole-program optimization pass. Memory operations, control flow, packet emission,
I/Q publication and nontrivial lower operations are rejected. Source flag effects
are intentionally outside the extracted mathematical function; callers must prove
that dropping them is legal before installing a replacement. No production path
currently installs one. Every manifest therefore says `"installable": false`.

`--native-f32` is required. This changes numeric behavior from PS2 VU arithmetic;
it is not a promise of equivalent rounding, exceptional values or flag behavior.
Generated LLVM uses ordinary add/subtract/multiply without fast-math, reassociation
or contraction flags. GPU denormal behavior and contraction must still be checked
in the emitted SPIR-V and on supported devices. Acceptance for a render region is
not permission to change gameplay or simulation arithmetic.

The four-pair original synthetic matrix fixture has 16 shared uniform words, four
per-vertex words, four output words and 28 live scalar expressions. The native
LLVM execution test compares 8,194 transforms, with varied finite mantissas and
exponents and two uniform sets, against an independent float32 C++ calculation.
It also checks inactive invocation bounds. Other tests cover masked writes,
broadcast overwrite hazards, accumulator subtraction, common expression sharing,
vf0, invalid interfaces and rejected side effects. This is not a PS2 equivalence
or game compatibility test.

The CPU object was compiled and executed with Clang 17 on Linux x86-64 in Release;
inspection showed packed SIMD multiplies/adds and no external helper calls. Three
related CTest tests passed. Vulkan-target LLVM was emitted but SPIR-V compilation,
validation and GPU execution were not performed in that environment. The normal
Windows build and commercial menu replay were not tested. No speedup is claimed.

## Build and inspect the experiment

A dependency-light mode builds the same conversion libraries without the Windows,
SDL or Vulkan runtime. It does not build AnyPS2's game executable:

```sh
cmake -S . -B work/render-research -DCMAKE_BUILD_TYPE=Release -DANYPS2_RENDER_RESEARCH_ONLY=ON
cmake --build work/render-research --config Release
ctest --test-dir work/render-research -C Release --output-on-failure -R '^render-'
```

Clang is required for `render-kernel-llvm-execution`; configuration reports when
that test is unavailable. The build emits original synthetic fixture files in the
build directory. On a single-configuration generator:

```sh
work/render-research/ps2-vu-render-probe work/render-research/render-kernel-fixture.bin 1 0 6 manifest --native-f32 1 2 3 4
```

For multi-configuration generators the executable is in the configuration folder.
The arguments are input region, VU unit, destination pair index, output vector,
output format, numerical consent, then uniform vector indices. Output formats are
`manifest`, `cpu-llvm` and `spirv-llvm`. Redirect stdout to retain generated text.
A private input must already be an isolated arithmetic region, not a complete ELF,
ISO or VU launch. No automatic menu extraction is implemented.

`ANYPS2_TEST_RENDER_SPIRV=ON` uses the existing LLVM/SPIR-V build chain and runs
`spirv-val`. This is an optional compilation/validation gate, not a GPU execution
test. It needs a compatible LLVM SPIR-V backend and associated tools. Do not mark
a kernel deployable just because source emission succeeds.

### Current kernel ABI

Descriptor set 0 has storage-buffer binding 0 for inputs, binding 1 for outputs,
and binding 2 for uniforms when present. Input word zero is the item count;
remaining words are packed per-item inputs. Uniform words are stored once per
batch. Outputs have no status-word prefix. Manifest input entries give the packed
word offsets; clients must use them rather than assume register-bank layout.

The native entry is `nt_render_reference(input, uniforms, output, item)`.
Compute workgroups have 64 invocations; launch enough groups for the item count.
Counts above 65,536 and invocations outside the count perform no output writes.
This guard is not allocation validation: the submitting owner must check all
buffer byte extents, non-overlap, descriptor bounds, lifetime and overflow before
launching. Output is a tightly packed stream, not yet connected to Vulkan draws.

## Target compiler architecture

The GS already rasterizes; VU work is not simply a substitute rasterizer. Treat
VU regions as general vector programs that may transform geometry, light it,
perform clipping, build variable-length packets or communicate results. A modern
vertex shader is appropriate only for a proven independent per-vertex function.

The intended pipeline is:

```
EE producers + VIF uploads + VU code + graphics consumers
    -> control/dataflow, memory SSA, source and resource provenance
    -> rendering regions with explicit inputs, outputs and effects
    -> semantic render IR + dependency graph
    -> CPU LLVM / shader LLVM -> validated SPIR-V
    -> persistent Vulkan resources + graphics/compute submissions
```

Recover at the producer side, before perspective division, viewport conversion
and packet packing discard information. A stream of final screen-space vertices
cannot generally reveal its original matrices, world coordinates or materials.
Preserve provenance without inventing meanings: an unknown transform remains an
unknown uniform array until analysis establishes its role.

Use existing boundaries rather than a parallel hardware implementation:

- `libraries/vu` supplies framing, control flow and instruction semantics. Add
  cross-block SSA, loop and memory analysis here or in a cohesive analysis library.
- `libraries/render_codegen` owns render-region extraction and generated kernels.
  The current pure arithmetic probe is the first narrow component.
- `libraries/gpu_codegen` and `cmake/llvm_shaders.cmake` provide shared arithmetic
  IR validation and ahead-of-time SPIR-V generation. Do not introduce runtime JIT.
- `libraries/graphics/src/native_raster.cpp` already owns Vulkan resources,
  submission and caches. Extend this ownership model with native draw inputs;
  do not round-trip a modern kernel's output through emulated graphics storage.

No game names, disc IDs, magic PCs or known matrices may select behavior. Program
identities may cache analyzed artifacts, but are not semantic proof or title hacks.

## Required semantic render IR

These are proposed contracts, not implemented runtime interfaces in this commit.

| Object or operation | Required information |
| --- | --- |
| Buffer/Image handle | Allocation identity, generation, size/format, lifetime and usage |
| Upload/Update | Destination range, owned source bytes, source revision and completion |
| Image view | Storage identity, subresource range, interpretation and alias relationship |
| Sampler | Min/mag/mip filters, addressing, LOD and explicit unsupported modes |
| Vertex/Index stream | Buffer handle, offset, stride, format, index type and valid extent |
| Uniform binding | Proven batch-invariant values, offsets, versions and lifetime |
| Shader/Kernel | Dataflow identity, IO schema, numeric policy and validated artifact |
| Pipeline | Shader variants, topology, target formats, depth, blending and write masks |
| Draw/Dispatch | Counts, bounds, resource uses, ordered outputs and completion token |

A handle is not a guest address. Reusing an address must not return an older image
or buffer. Track allocation generations and overlapping storage, including palette
updates, packed formats and render-target views. Cache immutable content by
validated identity plus interpretation, not by pointer alone. Sampler state is
separate from texture data; changing filtering should not re-upload a texture.
Some source wrap/filter modes need shader logic rather than a direct VkSampler.

Persist vertex/index data and textures across frames when their producers have
not changed. Update only dirty ranges. Separate per-frame transforms from static
geometry. Track resource retirement by GPU completion, not CPU submission return.

## Choosing CPU, vertex or compute execution

**Independent, fixed-output transforms:** prove iteration independence, recover
attribute streams and batch-invariant inputs, then emit a vertex function. Keep
clip-space position available and let the modern graphics pipeline rasterize.
Do not assume an arbitrary register is a position or matrix.

**Variable-output geometry:** clipping, strip expansion or other bounded topology
changes can use compute to write vertex/index buffers and indirect draw commands.
Use bounded allocation and stable ordering; prefix sums are one option when output
counts vary. A storage buffer can also be a vertex buffer with the correct usage
flags. No GPU-to-CPU readback is needed merely to pass compute output to drawing.

**CPU-observed or tiny work:** retain compiled native CPU work when observable
results, synchronization or launch cost makes GPU execution unsuitable. This is
not an interpreter fallback. Small 2D menus may be faster as batched native draws
than as many compute launches. Select at conversion/scheduling time from proven
effects and measured workload costs, not from a game's title.

Do not map each VU block or instruction to a shader dispatch. Recover and fuse
meaningful regions. A launch-local register bank and polling loop copied to a
shader would preserve the overhead this project is trying to remove.

## Analysis and correctness gates

A region replacement needs all of the following before runtime installation:

1. Complete reachable control flow, including delay-slot and continuation effects.
   Recover loop bounds and induction variables; retain a diagnostic for ambiguity.
2. Memory access functions and alias/dependence analysis across iterations. Recover
   VIF unpack formats, strides and double-buffer ownership rather than assuming
   that a contiguous memory range is a vertex array.
3. Backward slices from all observable outputs: graphics, CPU-readable memory,
   flags, live registers, synchronization and subsequent launches. Eliminate only
   unobserved state. A flag used for clipping remains a computation dependency.
4. Proven input rates and output schema. User annotations in this prototype do not
   supply these proofs. Packet construction may be removed only after its complete
   consumer behavior is represented by native draw/resource operations.
5. Explicit numeric policy and per-region acceptance. Keep precision-sensitive
   regions on compiled native operations until an allowed replacement is verified.
6. Validated shaders, runtime resource bounds and dependency ordering. The Vulkan
   path must observe previous uploads and writes without broad idle waits.

Never silently manufacture a matrix/material or treat observed execution as proof
that unobserved branches cannot occur. Conversion diagnostics should identify the
missing proof and affected region, not silently substitute a success result.

## Synchronization and performance

Build dependencies from resource ranges and actual consumers. Compute writes
consumed as vertices need the corresponding vertex-input dependency; generated
indirect commands need indirect-command visibility. Queue ownership transfers are
required when using distinct queue families. Keep passes on one suitable queue
until measurements justify asynchronous compute complexity.

Preserve transparent draw order, framebuffer feedback and CPU-visible completion.
Do not globally sort by texture merely to improve batching. FINISH must correspond
to submitted GPU work completing, not recording commands. CPU readbacks are explicit
edges; avoid per-launch waits and full-surface downloads when only later GPU work
consumes the result. Reuse pipeline/descriptor allocations and stage uploads through
bounded persistent arenas with fences or timeline-based retirement.

Benchmark the old and new render paths with identical simulation progress, not
just window presentation rate. Record CPU submission and VU time, GPU time, frame
P50/P95/P99, uploaded/read-back bytes, dispatch/draw counts, cache hit rates and
peak memory. Include small menus and larger 3D batches. A synthetic arithmetic
speedup or fewer instructions is not an end-to-end result.

## Menu test strategy and completion milestones

A menu is not necessarily one extractable function: it depends on assets, threads,
input and previous frames. The useful replay boundary is a complete render job or
frame with owned inputs, not an arbitrary guest PC. A future opt-in capture must
include compiled-program identities, input ranges/versions, uniforms, texture and
palette versions, ordered draw state, completion/readback dependencies and expected
outputs. Capture several histories: initial menu, animated menu object, entering
settings and returning. Keep captures and generated game artifacts under ignored
`work/`; never commit commercial data.

### Measured submission structure

`ps2-native --capture-vectors` (on `main`) records accepted vector submissions
per displayed frame. A private capture of a 3D menu scene, with the program
running on compiled native VU code, showed the conventional producer structure
this design assumes:

- About 1,800 VU1 launches per frame from about a dozen program entries, each
  preceded by a few data unpacks: roughly 10,000 unpacks and 2 MiB per frame,
  with no microcode uploads and little direct graphics data.
- About 93% of unpacked bytes were identical to the previous frame. They were
  vertex-like streams (three- and two-component 32-bit words, packed 8/16-bit
  four-component data) written to rotating input buffers.
- The changing remainder was small four-component float blocks (3-16 quadwords)
  written to a few fixed destinations: per-object parameter blocks.
- Every hot VU block was the body of a backward-branching loop in a program that
  ends batches with graphics kicks.

This motivates, in order: content-identified resident GPU streams for unchanged
unpacks; per-launch parameter blocks as uniforms; and lifting the per-vertex loops
to vertex shaders. Caching uploads by content is semantics-preserving on its own,
but the CPU saving requires the lifted program to run on the GPU. Memoizing whole
launches is not sufficient here: the parameters change every frame in an animated
scene, so outputs change even when streams do not. Role assignment must still come
from program dataflow, not from observed change rates.

The present loop frontend rejects all of these loops at its first check: they use
branch-pair and delay-slot work, other counter forms, auto-increment memory, the
I register, EFU and conversion operations, and MAC-flag tests. Gate 1 below must
cover these generally.

The next implementation gates are concrete:

1. Extend analysis to one bounded vertex loop with memory loads and stores, and
   prove live outputs/flags and uniform origins using original synthetic fixtures.
2. Add resource-backed render jobs and a headless textured-menu fixture with a
   transformed object. Compare the existing raster path and lifted output, including
   ordering, dynamic texture updates, aliasing and failed submissions.
3. Compile/validate shader artifacts and execute them on Vulkan. Compare intermediate
   values plus rendered output under the declared numeric policy. Keep zero GPU
   validation errors, but do not treat that alone as semantic correctness.
4. Implement private capture/replay, then exercise real menu transitions using the
   user's local input. Record coverage and failures rather than declaring a boot
   from a single frame. Run the normal Windows build and affected existing tests.
5. Replace packet production for accepted regions with direct persistent draws;
   benchmark before enabling any production path. Unknown regions remain compiled
   native code with explicit diagnostics, never an instruction interpreter.

RTX-oriented world geometry, previous transforms, camera and material roles are
later consumers of recovered provenance. The current arithmetic graph does not
provide those scene semantics or implement ray tracing.

## Public references and provenance

No external implementation was copied. The prototype reuses existing AnyPS2 code
and contains only original synthetic instruction fixtures. The design uses these
public API references (consulted 2026-11-04):

- [Khronos Vulkan compute tutorial](https://docs.vulkan.org/tutorial/latest/11_Compute_Shader.html): shared compute/vertex buffer usage.
- [Khronos multi-draw indirect sample](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html): GPU-produced indirect draws and capability checks.
- [Khronos synchronization examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples): dependencies between compute writes and graphics/indirect consumers.

These API references are not evidence that the proposed runtime integration or
commercial compatibility has been implemented.
