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

prints one line per loop. On a private commercial VU1 image, the hot loops had
affine counters and constant-stride streams with large invariant register sets
and no packet or flag effects; their remaining blocker was software pipelining
(values and the accumulator handed to the next iteration). The next frontend step
is to recover the per-element function from such modulo-scheduled loops by
symbolically executing several iterations and proving that each store depends
only on fixed-offset stream elements and invariants.

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

## Validation and limits

Two original synthetic loops vary register allocation, instruction destination,
source offsets and vertex/output strides. Both generated CPU functions were built
with Clang 17 at O3 and executed on Linux x86-64: 8,256 transforms per variant
matched an independent float32 C++ reference bit-for-bit. Uniform data and vertex
inputs vary between batches. Tests also check input immutability, output canaries,
invalid counts, unsafe dependencies and malformed loop shapes. The complete local
research suite passed eight Release CTest tests, including the original three.
The source mirror used Git-blob-verified dependencies; a full repository clone was
unavailable in the validation environment. The normal Windows build was not run.

Object inspection found packed SIMD multiply/add instructions and no undefined
helper symbols. This is evidence of native code generation, not a frame-time or
GPU speedup. No end-to-end performance improvement is claimed.

Vulkan-target LLVM was emitted and structurally checked. An LLVM SPIR-V backend,
SPIR-V validator and Vulkan development toolchain were unavailable here; shader
compilation, GPU execution, Vulkan draws and GT4 menu replay are not established.
Numerical policy is explicit native float32, not PS2 bit-exact arithmetic. Shader
rounding/contraction and visual acceptance still require device-level validation.

The next integration boundary is a validated GPU-resident stream feeding an actual
draw, with resource ownership and synchronization tested. No persistent resource
manager, automatic replacement installation or ray tracing is implemented here.
