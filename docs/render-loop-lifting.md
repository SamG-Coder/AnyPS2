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
value, so the same compiled kernel serves any batch position and buffer. Parameter
slot 18 numbers output records from a first record, after any startup prefix.

When the loop has flag-producing arithmetic (ADD/SUB/MUL/MADD/MSUB forms), pipeline
recovery also keeps every such instruction's written lanes, including results that
are never stored, and requires them to reach the same steady state as the stores.
Each record then ends with a sticky-flag word: the status bits 6-11 that the
element's iteration sets (zero, sign, underflow and overflow of the written lanes,
plus MADD/MSUB product flags), computed from the exact helpers. Outer-product
forms reduce a MAC word that keeps earlier lane flags and are not recovered.

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
position write at the end of each vertex record. ST must precede RGBAQ. Fog is
supported; textures, continued primitives and unsupported layouts produce diagnostics. A batch
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

`NativeDrawing::assemble_triangles_on_gpu(renderer)` applies the same assembly to
ordinary kicked packets: triangle lists, strips and fans, and sprite lists, plain,
textured (UV or validated perspective STQ, including indexed textures with a
palette snapshot) and fogged. Each GIF tag is processed in runs of vertex-register
entries (RGBAQ, ST, UV, FOG, positions, NOP, and A+D writes to those registers);
any other register write ends a run and applies normally, as a modern renderer
splits batches at state changes. A run is planned from the GS state at its start.
The GS state machine still applies every register, with draw planning suppressed,
and records each vertex as it resolves at a position write (color, Q, S, T, UV,
fog, position, depth, kick). These resolved records make PACKED, REGLIST and A+D
encodings, register order, several vertices per loop and values carried from
earlier vertices irrelevant to the GPU. A failed host check (ST/Q range, textured
sprite extent, primitive limit) replays the run through the CPU planner from a
state snapshot. Consecutive runs with identical draw state join one batch; joined
strips carry a restart flag so that no triangle spans the join. A batch is
bounded by all of its vertices, as `plan_sprite` bounds a primitive, and textures
that may alias the color or depth target keep the CPU planner. Batches extend the
renderer's open instanced draw like consecutive CPU draws. When a frame slot is
submitted, its staging (records, plans and a batch table) is copied once to
device memory and a single assembly dispatch covers every host-record batch of the
slot; element-kernel launches come first in the same command buffer, which is
submitted before the slot's draws. Draw records live in device-local memory the
host can map, so assembly writes and draw reads stay in video memory. `ps2-native` uses the path by
default when the renderer has ordered fragment access (`--cpu-triangles` disables
it, `--gpu-triangles` requires it); the graphics profile reports
`gpu_triangle_batches`, fallback reasons and `vector_gpu_ms`.

A private local capture of 9,143 GT4 graphics batches replayed through the CPU
planner and through this path produced identical complete GS memory at 91
checkpoints and at the end; 11,015 GPU batches covered all but 263 vertex runs
(textures that might alias their target). In repeated headless 60-second GT4 runs
on one host, 5-second windows issued the same number of draw calls as the CPU
planner. Render-thread time was within run-to-run variation of the CPU planner
(about 0.74-0.76 s against 0.71-0.74 s): command submission and draw recording
took less time, GS packet parsing slightly more. Draw passes took about 65 ms of
GPU time against 73 ms, plus about 74 ms of assembly. No frame-rate or input-lag
improvement is claimed from these headless runs.

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
accepts an affine countdown-to-zero loop and one forward output stream. When the
pair before the backedge updates the counter, the conditional branch compares the
backed-up previous value (the VU's one-pair integer backup, shared with the VU code
generator through `branch_backup_register`), so the loop runs counter/step+1 times;
a zero counter is then a valid single iteration and the final update's wrap below
zero is executed by the original replayed iteration. It rejects overlapping output records, integer stores and
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

## Native region execution

`analyze_vector_region(image, unit, entries, head)` requires the complete declared
image and export entries. It rejects a head that executes as a branch delay slot
and an undeclared exit. Other paths into the loop body (interior exports, direct
or register jumps, launch-supplied ITOP targets) need no proof: the service only
replaces execution that reaches the head as an ordinary pair, after validating
the complete architectural state there, and reproduces running the loop from the
head to its exit. Execution entering elsewhere runs the original compiled blocks
until it reaches the head. The current subset accepts
loads/stores, vector moves/rotations, ABS, MIN/MAX, integer/float conversions,
ADD/SUB/MUL/MADD/MSUB forms writing VF lanes or the accumulator (including Q
operands), self-updating integer counters, DIV/SQRT/RSQRT with WAITQ, and the EFU
operations ERLENG/ESIN/ESQRT with WAITP and MFP. Outer products, clipping, kicks
and other unproved side effects are rejected. Lane dependency analysis must prove
that a bounded number of final iterations reconstructs every changed VF and
accumulator lane and the Q and P pipelines; Q/P results become visible after their
issue latency, and a producer or wait publishes the earlier result before its pair
reads Q or P, as in the VU code generator. At most eight iterations are replayed.

With flag arithmetic, flags computed by those reconstructing iterations come from
registers that are not yet exact, so they are discarded: the replay then clears the
sticky bits and MAC queue and runs two further iterations on exact registers, which
republish the queue, MAC and visible values. Sticky bits of the entry, the startup
iterations and every steady element before that exact tail come from the startup
state and the kernel records; they precede the tail and are therefore also visible.

`ps2-vu-loop-probe --region <image> <unit> <destination> <head>
boundary|elements|binding <entries...>` emits each reproducible LLVM module.
`emit_vector_region_llvm` provides the same boundary, element and binding modules.
The binding is an immutable `VectorRegion` with native linker relocations to its
iteration, element and guard functions. `execute_vector_region` validates the
entry and quiescent pipelines, runs the original AOT startup, evaluates recovered
steady-state outputs, and reconstructs live-out state with the bounded original
AOT tail. Short loops run entirely through their compiled boundary function.
The service commits registers, memory and continuation only after every stage
succeeds. It contains no instruction decoder or original-address dispatch table.

This uses the existing VU state ABI as a migration boundary, not the final native
render ABI. Output memory remains observable and is preserved. The optional
synchronous executor can run the AOT element kernel on Vulkan, but must return
completed output words; the GPU test does so by readback.

`prepare_region_launch` is the device-resident form. It performs the same entry
validation, AOT startup and live-out reconstruction, but leaves the steady-state
output rows to the device. Its `RegionLaunch` holds the entry data memory, launch
parameters (slot 18 numbers output records after the startup elements) and the
startup records as a `VectorLaunch::prefix`. With the conversion-time PACKED
binding from `bind_element_packet`, `NativeDrawing::submit_vectors` draws every
element without a CPU copy of steady outputs. Those rows are GPU-owned: a consumer
other than the draw must first call `materialize_region_outputs`, which runs the
same AOT kernel and yields exactly the `execute_vector_region` state. Batches of
fewer than three vertices are rejected for drawing, because they would leave
GPU-owned vertex-queue state; such regions are materialized and kicked normally.
Deciding automatically that only a graphics kick consumes the rows remains open.
No speed claim is made.

Entries need not be quiescent. Heads reached during execution carry the loop's
own in-flight FMAC/clip results and the integer branch backup; the service runs
enough original iterations (FMAC/clip results publish within four pairs) before
the first element, and the first pair at a head replaces the backup. A pending or
delayed Q or P result is absorbed the same way when the loop issues that unit
itself (the region's `pipelines` bits): its first issue publishes the earlier
result inside the original first iteration, and steady elements are proved
entry-independent. A loop that only reads Q or P, and a pending kick, still
reject such entries.

## Converted programs

`ps2-vu-loop-probe --regions <whole-entry-image> <unit> <destination> <directory>`
analyzes every loop of an image, writes each accepted region's boundary, element
and binding modules, a head-to-binding table and `regions.tsv` with each loop's
acceptance or rejection reason. A converted project links that directory with
`ANYPS2_VU1_REGIONS`; the VU program then carries loop substitutes, a typed
callback the dispatcher invokes when execution enters a listed head from another
block. `ps2-native --vu-regions check` validates each such entry and reports the
reasons regions decline it while running the original code; `--vu-regions cpu`
executes accepted entries with the region service. The CPU service is slower than
the original blocks and exists for in-program exactness checks; the GPU-owned
launch path is not yet installed.

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

Region tests compare complete state and memory against original LLVM-compiled VU
iterations for VU0/VU1, short loops, pipelined startup and multi-iteration live-out
dependencies. `render-region-drawing` draws each region (list, strip and fan, 1-60
vertices) with GPU-owned outputs and compares framebuffer storage with CPU region
execution followed by an ordinary packet kick; materialized state must equal the
CPU state. Both CPU and Vulkan executors are exercised, including rejected
entry/range conditions and transactional failure. The runtime executable links
native services and generated objects without the analysis/decoder libraries.

Element outputs may use up to eight forward streams, one per VI base; the launch
guard keeps every stream in bounds and the streams disjoint. A read whose stride
equals every store of a stream may overlap that stream when, at launch, each
(load, store) pair either never touches a common row, writes it in a later
iteration, or, in the same iteration, loads before storing: every row is then
read before it is written, so in-place transforms (also through different VI
registers) use the entry memory the kernel receives.
On a local commercial VU1 program, 17 of 49 loops are accepted as regions,
including its most expensive loop in a 3D menu; a local validator matched 3,400
randomized region executions against the original compiled loops in complete VU
state. A Release timing of those loops on one host
found the CPU region service 1.2-13 times slower than the original compiled blocks,
which already use inlined four-lane helpers, while the element kernels call scalar
exact helpers. Regions must therefore not be installed as a CPU replacement. The
device-resident preparation (`prepare_region_launch`) took a roughly constant
1.1-4.9 us per launch, faster than the original loop only for longer batches (for
example 4.3 us against 27.2 us at 64 vertices for the slowest loop), and it also
avoids per-primitive packet planning. An installation should select the GPU-owned
path from the actual launch count, after measuring the complete frame.

The GPU-owned draw path is exercised by this explicit interface, not installed
into converted games. The region proofs apply only to the accepted subset above;
other arithmetic, side effects and renderer ownership remain unsupported. There is
no GT4 performance or gameplay claim from these synthetic comparisons. The
older `--native-f32` loop probe has a different, explicitly approximate numeric
policy; it must not be substituted for the exact element kernels silently.
