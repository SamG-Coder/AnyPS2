# Bounded vertex-loop lifting

The `ps2-vu-loop-probe` frontend extends the arithmetic probe described in
[render modernization](render-modernization.md). It recovers a narrow complete
countdown loop, its memory streams and uniform rates, then emits ordinary
native-float32 LLVM. It is not installed in generated games and does not execute
VU instructions at runtime. Production rendering is unchanged.

## Accepted region

The input is an isolated, contiguous region of 7-256 VU instruction pairs:

1. Full-quad loads into distinct vectors, with invariant addresses during the
   iteration, followed by at least one neutral pair.
2. A straight-line arithmetic slice supported by `lift_vu_arithmetic`.
3. One full-quad store, followed by constant forward pointer increments and a
   countdown decrement of one. Each integer is updated at most once.
4. Two neutral pairs, a BNE backedge to the first pair comparing the countdown
   with vi0, and a neutral branch delay slot.

No game identifier, address constant, matrix register convention or shader name
selects this behavior. VU framing and decoding are reused. The analyser derives
memory strides from induction updates, finds the stored output vector, and marks
unchanged input streams as uniforms. The matrix fixture is just one computation;
no matrix pattern is required. Registers not loaded within the iteration and
live entry-accumulator dependencies are rejected rather than assumed uniform.

This first frontend intentionally rejects mixed load/arithmetic scheduling,
auto-increment memory operations, nested/data-dependent branches, clipping,
packet emission, I/Q publication, partial memory transfers and control flags.
Later passes may broaden those cases using actual dependency analysis; they must
not delete an unsupported operation just because it appears locally dead.

## General loop structure analysis

`analyze_loop_structure` reports the structure of any conditional backward loop
instead of matching one instruction order. It symbolically executes one iteration,
including the branch delay slot, with integer registers as entry register plus
constant. It recovers induction steps and the tested counter, memory accesses with
per-iteration strides (including post-increment and pre-decrement forms), vectors
read before written (live-in), loop invariants, and accumulator, Q, P and clip
values carried to the next iteration. Graphics kicks, internal branches, flag
tests, non-affine addresses and stationary stores are listed as blockers. Values
whose iteration-final definition is a load are reported as prefetched streams.

The report is evidence for lifting, not installability: pipeline latency between
units is not modeled, and entry state, live-out registers and flags, and external
entries still need separate proofs. `find_loop_regions` lists every backward
conditional branch in a program image.

```sh
ps2-vu-loop-probe --structure <program-image.bin> <unit> <destination-index>
```

prints one line per loop, including the pipeline recovery result below.

## Software-pipeline recovery

`recover_pipelined_loop` handles loops whose only blockers are values, the
accumulator, Q or P carried to the next iteration. It symbolically executes ten
iterations over a hash-consed lane-level expression graph with exact VU operation
nodes (no host float semantics). VF reads are sequential, as VU hazards stall;
Q and P publication follows the production generator's latencies (7/13 and
12/24/29 pairs, published early by WAITQ/WAITP or a new producer). Loads that read
a row stored by an earlier iteration are rejected. A store site's steady state
starts where its value no longer depends on loop-entry state and each iteration's
value equals the previous one with every stream row advanced by its stride. The
result is a per-element function: element k is stored by iteration k, with load
and store rows relative to each base register's entry value plus stride times k.

On a private commercial VU1 image, 21 of 53 loops were recovered, including every
hot loop without internal branches. A local validator ran each recovered loop's
production-compiled VU block iteration by iteration on random data and compared
38,700 stored lanes of steady-state elements against the recovered functions
evaluated with the exact numeric helpers: all matched bit for bit. Prologue and
epilogue equivalence (elements before the steady state and after the last
iteration), loops with internal branches, live-out registers and flags, and
installation remain open.

## Element kernels

`emit_element_kernel_llvm` compiles a recovered per-element function to LLVM, as a
CPU function or a Vulkan compute shader with one element per invocation and 64
invocations per workgroup. Every operation node calls an exact `nt_gpu_element_*`
helper from `gpu_numeric.cpp`, the same definitions the CPU VU path uses, so both
variants reproduce VU results bit for bit. They are not native-float kernels.

| Binding | Data |
| --- | --- |
| 0 | Read-only VU data memory snapshot, four words per row |
| 1 | Output: per element, a status word then one word per recovered store |
| 2 | Read-only parameters from `element_parameters`: count, first element, base rows, entry vectors, ACC, I, Q, P |

The CPU entry is `void nt_render_elements(const uint32_t* data, const uint32_t* parameters, uint32_t* output)`.
Element item covers element first+item; rows wrap at the unit's data size. A
nonzero status word marks an element with an operand the VU would stop on; its
stored words must not be used. Loads are relative to each base register's entry
value, so the same compiled kernel serves any batch position and buffer.

```sh
ps2-vu-loop-probe --element <program-image.bin> <unit> <destination-index> <loop-head> cpu|vulkan
```

Shaders are compiled with `anyps2_compile_llvm_shader(... STRUCTURIZE)`. With the
LLVM 22 SPIR-V backend that requires `lower-switch` before `structurizecfg`
(otherwise a switch becomes a branch on undef, and drivers can reduce the shader to
one that writes nothing) and `-disable-machine-cse` (otherwise an instruction can
land between `OpSelectionMerge` and its branch). Numeric helpers avoid wide struct
returns and private arrays, which this backend cannot lower.

On the private image above, all 21 recovered loops compiled to valid SPIR-V. A local
Vulkan run on an NVIDIA RTX 5080 matched the CPU kernels, which match the production
VU blocks, word for word. Repository tests use an original synthetic reciprocal
square-root loop: the CPU kernel is executed against the exact arithmetic definitions,
and with `ANYPS2_TEST_LLVM_GPU=ON` the shader runs on a Vulkan device, including
wrapped rows, VU-stop operands, flag-only cases and unused invocations. Prologue and
epilogue elements, branchy loops and automatic replacement installation remain open.
The explicit resident draw interface below accepts already compiled element kernels.
With LLVM 22.1.8 the two native-float32 loop shaders below crash `llc`; the element
shader is unaffected.

## Conditional independence and binding

`analyze_vertex_loop` returns an immutable `VertexLoop`. `bind_vertex_loop` takes
an entry integer-register snapshot and a source-word span. These are conversion
or capture inputs, not the compiled function's ABI. Binding checks all read and
write extents, including dead reads, and rejects integer/address wrap and overlap
between the output and any input/uniform range. Range envelopes deliberately
reject some otherwise legal interleaving. Every output row is distinct.

The original loop is do/while. Entry count zero is rejected, not reinterpreted
as an empty source loop. Bounds are limited to VU0/VU1 data capacities. The emitted
kernel also writes nothing for invalid counts, but that guard is not a substitute
for checking descriptor sizes and ownership before invocation.

The successful binding establishes independent memory results for this isolated
loop under the declared float32 policy. It does NOT establish that live-out vector,
integer, accumulator or flag state is unobserved, that an incoming execution pipeline
is quiescent, or that the region has no external entry. Those remain required
integration proofs. Manifests therefore retain `"installable": false`.

## Compiled stream ABI

Vulkan descriptor set 0 is prepared for these storage buffers:

| Binding | Data |
| --- | --- |
| 0 | Read-only source attributes in their original strided layout |
| 1 | Disjoint packed output: four float32 words per item |
| 2 | Read-only parameters: item count, then one word per live input |

For a per-vertex input the parameter word is its first source-word offset; the
recovered word stride is compiled into the kernel. For a uniform input it contains
the shared float32 bits. The manifest records the parameter ordering and original
output-row mapping. A binding is not a resource handle: the caller still owns
allocation, immutable snapshots, upload completion and resource retirement.

There is no per-item input packing and no matrix duplication across vertices.
A uniform update only changes the small parameter block. Output is a packed buffer
intended for a later draw consumer, not a rebuilt VU register bank or GIF packet.
Its semantic role, such as clip-space position, is NOT guessed.

The native entry is:

```cpp
void nt_render_loop(const uint32_t* source,
                    const uint32_t* parameters,
                    uint32_t* output);
```

It processes the complete batch in one call, with uniform loads outside the loop.
All three buffers must be non-overlapping, properly sized and alive until work
completes; source and parameter bytes must remain immutable. The Vulkan-target
variant uses one item per invocation and 64 invocations per workgroup. Conversion
emits LLVM only; runtime instruction decoding and JIT compilation are not added.

## Build and inspect

Use the existing research-only configuration:

```sh
cmake -S . -B work/render-research -DCMAKE_BUILD_TYPE=Release -DANYPS2_RENDER_RESEARCH_ONLY=ON
cmake --build work/render-research --config Release
ctest --test-dir work/render-research -C Release --output-on-failure -R '^render-'
work/render-research/ps2-vu-loop-probe work/render-research/render-loop-0.bin 1 0 manifest --native-f32
```

Multi-configuration generators put the executable in the configuration directory.
Output formats are `manifest`, `cpu-llvm` and `spirv-llvm`. The second synthetic
fixture uses destination index 112. No output-vector or uniform annotations are
needed. The input must already be a complete isolated loop, not a game ELF or ISO.
Automatic menu capture and whole-program region discovery remain unimplemented.

`ANYPS2_TEST_RENDER_SPIRV=ON` also compiles and validates both loop shaders through
the existing LLVM/SPIR-V build chain. That option does not run a GPU or draw a frame.

## Compiled packet bindings

`bind_element_packet` maps recovered store rows and lanes to a declared PACKED
GIF tag. Its inputs include the proven symbolic payload base and first row; it
does not infer a tag or payload origin from vertex values. Store emission order
and register allocation do not determine the mapping. Constant stores become
constant layout words. Duplicate writes, missing consumed lanes, mismatched
streams/strides, unsupported register layouts and implicit PRE updates are
rejected. Unused PACKED padding may be absent.

`emit_element_packet_llvm` emits immutable native layout symbols, the output
stride and the minimum steady-state element. These link alongside the compiled
CPU code; the runtime consumes the layout without analyzing VU instructions.
The draw regression uses this emitted binding with deliberately reordered stores.
The probe can emit the binding separately:

```sh
ps2-vu-loop-probe --element-binding <program-image.bin> <unit> <destination> <loop-head> <base-register> <first-row> <tag-low-hex> <registers-hex>
```

This proves the store-to-packet mapping only. It does not prove runtime input/output
non-aliasing, loop counts, prologue/epilogue equivalence or dead live-out state.
Those checks still gate automatic installation in a converted program.

## GPU-resident drawing

`VulkanRenderer::load_vector_kernel(spirv, unit)` loads a build-time compiled
`emit_element_kernel_llvm` kernel. It requires Vulkan 1.3, `shaderInt8`, `shaderInt64`
and ordered fragment access. Ordinary CPU-packet rendering still works without
vector-kernel support. This API accepts trusted compiler output with the element
ABI above; it is not a general shader loader or a runtime VU translator. The
caller must match the kernel's unit, output stride and parameter layout.

`NativeDrawing::submit_vectors(renderer, launch)` consumes current GS state plus
an explicit PACKED vertex layout. Each layout word identifies an element store
or a constant. It accepts independent triangle lists, strips and fans with one
position write at the end of each vertex record. ST must precede RGBAQ. Texture,
fog, continued primitives and unsupported layouts produce diagnostics. A batch
is bounded to 4,096 primitive slots; wrapped or aliased target layouts outside
the renderer's supported bounds are rejected.

`NativeVectors` stages parameter and dynamic stream changes, retains
content-identified immutable streams, and applies ordered memory writes to a
zero-initialized per-unit data view. Unwritten rows retain their previous values.
The element kernel writes device-local results; a second compute pass assembles
native triangle records. `NativeRaster` ends the active render pass before this
work, inserts compute-to-draw dependencies, then binds the target using the
conservative scissor union and submits one instanced draw. Color/depth ordering
uses the existing ordered-fragment pipeline. No generated vertex packets or
framebuffer images make a CPU round trip during submission.

Frame slots own staging and status memory until their queue fence completes.
`RenderStatistics` exposes vector launches, resident hits/uploads, staged bytes
and failed-element counts. A failed element suppresses all primitives using it;
`vector_failures` becomes visible after completion collection (`synchronize` or
`completed_submission`). Callers must treat an increase as an incomplete batch,
not successful conversion. Primitive/draw counters count submitted slots,
including culled slots; they are not a count of visible triangles or game frames.

The final GS vertex latches remain GPU-owned. The planner marks registers written
by a vector batch unknown and rejects later CPU consumption until explicit writes
restore them. Strips, fans and incomplete lists also require a PRIM write before
another batch can consume their vertex queue. This is deliberate rejection, not
an assumed last-vertex value or hidden readback.

## Compiled launch-range guards

`ps2-vu-loop-probe --element <image> <unit> <destination> <loop-head> guard`
emits a native LLVM function `nt_render_element_guard(parameters)`. The converter
accepts an affine countdown-to-zero loop with a separated counter update and one
forward output stream. It rejects overlapping output records, integer stores and
unproved integer recurrences. The compiled guard checks the actual launch values:
positive divisible trip count, requested steady-state subrange, intermediate VI
values, memory bounds, and input/output separation. It includes original reads
as well as shifted loads in the recovered element graph.

Pass the linked function through `VectorLaunch::range_guard`. The renderer runs
it before recording the batch and reports count, startup, wrap or alias failures.
This is ordinary native code over launch parameters, with no runtime VU decoding
or translation. Explicit low-level kernel tests may omit the guard; recovered
region integrations must supply the matching compiler-generated function.

These checks are conservative: memory wrap and overlapping row envelopes are
rejected even when a more detailed lane analysis could prove them safe. Passing
this guard establishes only count/range eligibility. It does not replace proofs
of region entry, startup/drain equivalence, live-out state or graphics ordering.

## Validation and limits

The original synthetic loop fixtures compile and execute native LLVM functions;
the optional Vulkan tests compile the same recovered element graphs to SPIR-V.
The draw test compares complete framebuffer storage against the production CPU
packet planner for lists, strips and fans, including ADC, XYZ3, winding changes,
degenerate/off-scissor culling, flat/shaded color, coverage and depth. It also
checks second-frame resident hits, interleaved CPU/GPU draws, slot reuse, stale
GS-state diagnostics and failed-element reporting. Compiled launch guards are
checked with aliased/wrapped streams, invalid countdowns and pipelined startup.
It runs headlessly with Vulkan validation; no presentation window or commercial assets are needed.

In a normal build with the LLVM SPIR-V toolchain configured:

```sh
cmake -S . -B work/vector-draw -DANYPS2_TEST_LLVM_GPU=ON
cmake --build work/vector-draw --config Release --target render-vector-draw-tests
ctest --test-dir work/vector-draw -C Release --output-on-failure -R '^render-vector-drawing$'
```

The renderer is exercised by this explicit interface, not installed automatically
into converted games. Entry/exit state equivalence, external-entry exclusion and
live-out proofs remain prerequisites for replacing a whole VU region. There is
no GT4 performance or gameplay claim from these synthetic comparisons. The
older `--native-f32` loop probe has a different, explicitly approximate numeric
policy; it must not be substituted for the exact element kernels silently.
