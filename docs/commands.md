# Command reference

Every program AnyPS2 builds, and how to use it. Paths below assume a default build,
so programs are in `build\Release\`. Run any program with no arguments to print its
own usage. Keep game files and everything generated from them under the ignored
`work/` directory, and read the [disclaimer](../DISCLAIMER.md) first.

## Convert and play a game

```powershell
# Build the tools once.
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 8

# Convert your own disc image. The output directory must not already exist.
.\build\Release\AnyPS2.exe convert 'C:\path\to\owned-game.iso' work\game-native --jobs 4

# Run the result, passing its data file and the disc.
.\work\game-native\bin\ps2-native.exe .\work\game-native\bin\program.data --disc 'C:\path\to\owned-game.iso'
```

Conversion reports what it could not convert, and a finished build is not proof that
a game runs. A game that resets the IOP also needs a converted module bundle, passed
with [`--iop-reset-session`](#converted-iop-sessions). To rebuild after a change without
converting again:

```powershell
cmake --build work\game-native\build --config Release --parallel 4
```

See [batch conversion](batch-conversion.md) for the output layout and compiler settings.

## `AnyPS2`

`AnyPS2 <command> [arguments]` runs the matching program with the same arguments.

| Command | Runs | Use it to |
| --- | --- | --- |
| `convert` | `ps2-convert-inventory --build` | Convert and link a disc image |
| `inventory` | `ps2-convert-inventory` | Convert without linking |
| `import` | `ps2-import` | Extract a disc's files and find its boot program |
| `inspect` | `ps2-inspect` | Print information about an ELF |
| `inspect-iop` | `ps2-iop-inspect` | Report the code around an IOP stopping point |
| `unpack` | `ps2-unpack` | Unpack a packed executable to an ELF |
| `recompile` | `ps2-recompile` | Generate an EE project from an ELF |
| `iop-recompile` | `ps2-iop-recompile` | Generate an IOP project from modules |
| `build` | `ps2-build` | Build a generated EE project |

## `ps2-native`: run a converted game

```text
ps2-native <program.data> [operation-budget] [options]
```

`program.data` must be the file generated with this executable. The operation budget
defaults to 1000000, so give a large value such as `1000000000000` for a real session.

Exit codes: `0` the game asked to exit, `3` it stopped for any other reason, including
`--run-seconds` running out, and `4` the operation budget ran out. Exit code 3 is not
always an error: the `host_stop=` and `stop=` lines in the output say what happened.

### Everyday options

| Option | Effect |
| --- | --- |
| `--disc <image.iso>` | Disc image to read from |
| `--disc-media cd\|dvd` | Disc type to report to the game |
| `--fullscreen` | Start in borderless full screen. F11 or Alt+Enter switches while running |
| `--resolution-scale 1\|3\|5` | Samples per console pixel on each axis. Default 5; 1 draws only the console's own pixels |
| `--close-on-stop` | Close the window when the game stops |
| `--display-stop` | Keep the window open after a stop. This is the default |
| `--run-seconds <1..86400>` | Stop after this long, for measured runs |
| `--headless` | Run with no window |

### Converted IOP sessions

Available in builds linked with converted IOP modules.

| Option | Effect |
| --- | --- |
| `--iop-reset-session <module.data>` | Run the selected module bundle when the game resets the IOP. Needs `--disc` |
| `--iop-default-session <module.data>` | Start with the linked default modules |
| `--probe-iop-reset <module.data>` | Investigate: stop at the reset without acknowledging it |

### Scripted input

| Option | Effect |
| --- | --- |
| `--pad-script <file>` | Replace the controllers with timed presses on port 0 |
| `--input-latency` | With a pad script, report the updates and milliseconds until each press changes the image |

Each script line is `<start-ms> <hold-ms> <button>[+<button>...]`, timed from the start
of execution, and `#` starts a comment. Buttons are `select l3 r3 start up right down
left l2 r2 l1 r1 triangle circle cross square`. For example, `5000 100 start` holds Start
for 100 ms at 5 seconds. Timing follows host execution, so confirm from captured frames
that a script reached the screen you meant before comparing runs.

### Rendering choices

| Option | Effect |
| --- | --- |
| `--gpu-triangles` | Require GPU triangle assembly, and fail if it is unavailable. Otherwise it is used when available |
| `--cpu-triangles` | Plan triangles on the CPU |
| `--validate-graphics` | Enable Vulkan validation layers |
| `--verify-textures` | Check generated texture kernels against reference results |

### Profiling

| Option | Effect |
| --- | --- |
| `--profile-graphics` | Print a cost report every 5 seconds |
| `--sample-threads <file>` | Sample each thread's position in a heavy scene and write the samples at exit. Needs `--profile-graphics` |
| `--trace-services` | Print native service calls |

The game's performance counters follow the host clock by default. These options choose
something else, and only one can be used at a time:

| Option | Effect |
| --- | --- |
| `--native-thread-cycles` | Use host cycles for the calling thread |
| `--native-pmu` | Use host hardware performance counters |
| `--strict-performance` | Provide no host source for the counters |

In a `--profile-graphics` report, `display_buffer_changes` divided by the seconds in the
report and by two circuits is the closest measure of game frames per second.
`updates_per_second` counts display scanouts, not game frames.

### Captures

Each writes private game data, so keep the output under `work/`. Directories must be new.

| Option | Records |
| --- | --- |
| `--capture-graphics <dir>` | Submitted graphics data and frames, for `ps2-graphics-replay` |
| `--capture-graphics-when-heavy` | With `--capture-graphics`, start only in a heavy scene. Needs `--profile-graphics` |
| `--capture-display <dir>` | Display frames only. Also works with `--headless` |
| `--capture-packets <file>` | VIF and GIF transfers for `--replay-packets`: program uploads from the start, the rest once a scene is heavy, up to 1.5 GB. Needs `--profile-graphics` |
| `--capture-vectors <dir>` | `submissions.tsv`: uploads, unpacks, direct packets and launches for the latest 16 frames |
| `--capture-vu <dir>` | VU programs that were uploaded but not converted |
| `--vu-capture <new-file> <jobs> <start-ms>` | VU1 state at the start of up to `<jobs>` launches, beginning `<start-ms>` after launch |
| `--capture-stop <dir>` | State at a stop, usable as `ps2-recompile --observation` |

### Testing without the game

| Option | Effect |
| --- | --- |
| `--replay-packets <file>` | Run a `--capture-packets` file through the packet path and compiled VU programs, with no game code, window or GPU, then exit |
| `--vu-regions check\|cpu` | In builds linked with converted VU1 regions: `check` reports why regions decline while running the original code, `cpu` uses the regions |

```powershell
ps2-native.exe program.data 1000 --replay-packets work\packets.bin
```

The replay prints what the game's thread pays per channel and a hash of everything
drawn. Each submission finishes before the next, so the costs exclude queueing. A
change to this path must leave the hash unchanged. Add `--profile-graphics` for VU
thread time, or `--sample-threads <file>` to sample the replay. The recording starts
mid-game, so its VU memory differs from the original run: the hash compares builds and
says nothing about the original frames.

## `ps2-graphics-replay`: replay graphics without the game

```text
ps2-graphics-replay [--scale n] <packets.bin> [repeat] [surface.ppm:fbp:fbw:psm:width:height]
```

Runs a `--capture-graphics` recording through the renderer and prints its cost and a
hash of the resulting memory. Two builds with the same hash on one capture rendered it
identically, and the hash must also match across scales. The optional last argument
writes a rendered surface to an image. Use relative paths in it, because a Windows
drive colon would be read as a separator.

## Conversion tools

### `ps2-convert-inventory`

```text
ps2-convert-inventory <disc.iso> [new-output-directory] [--payloads mapping-file]
    [--no-compile] [--audit-only] [--jobs N] [--compiler path] [--build] [--source-root path]
```

| Option | Effect |
| --- | --- |
| `--build` | Also link convertible targets |
| `--no-compile` | Generate only |
| `--audit-only` | Report calls and coverage without generating or compiling game code |
| `--jobs N` | Compiler processes, 1 to 64. Default at most 4 |
| `--compiler <path>` | Clang to use |
| `--source-root <path>` | AnyPS2 source location, if not the one it was built from |
| `--payloads <file>` | Copied payload mappings |
| `--help` | Print usage |

### `ps2-import`

```text
ps2-import <disc.iso> <new-output-directory>
```

### `ps2-inspect`

```text
ps2-inspect <executable.elf> [--flow|--metrics [window]]
```

### `ps2-unpack`

```text
ps2-unpack <packed-image> <new-output.elf> [--layout classification.txt]
ps2-unpack --disc <disc.iso> <disc-path> <new-output.elf> [--layout classification.txt]
```

See [EE native conversion](ee-native.md#workflow) for the layout format.

### `ps2-recompile`

```text
ps2-recompile <boot.elf> <new-output-directory> [--verify-only] [--whole-code]
    [--function-symbols] [--payloads mapping-file] [--image secondary.elf ...]
    [--observation native-stop-directory ...]
```

| Option | Effect |
| --- | --- |
| `--verify-only` | Report unresolved flow without generating anything. Exit code 2 means gaps remain |
| `--whole-code` | Compile every declared EE code word, with no caller discovery |
| `--function-symbols` | Emit named function symbols |
| `--payloads <file>` | Copied payload mappings |
| `--image <file>` | Secondary executable image. Repeatable |
| `--observation <dir>` | A `--capture-stop` directory that names an indirect destination. Repeatable |

Use the same mappings and images for verification and generation. See
[flow verification](flow-verification.md) for the reports.

### `ps2-build`

```text
ps2-build <generated-project> <AnyPS2-source-root> [--vu <vu0.ll>] [--vu1 <vu1.ll>]
```

### `ps2-iop-recompile`

```text
ps2-iop-recompile <module.irx> <new-output-directory> <base-hex>
ps2-iop-recompile --bundle <new-output-directory> <base-hex> <module.irx> [<base-hex> <module.irx> ...]
```

In a bundle, prefix a base with `romdrv@` to replace a compatible ROMDRV 2.x module with
native archive services, or `sealed@` to compile a module for a later `LoadStartModule`
without placing it in the startup image. The two prefixes cannot be combined.

### `ps2-iop-inspect`

```text
ps2-iop-inspect <module.irx> <base-hex> <new-report.md> --anchor <hex> [--radius <hex>] [--capture <directory>]
ps2-iop-inspect --bundle <new-report.md> --capture <directory> [--radius <hex>] --modules <base> <module> ...
```

With `--capture` and no `--anchor`, it selects the observed PC or return call site.
The report contains private data. See [batch conversion](batch-conversion.md).

## VU tools

Unit is `0` or `1`, and destinations are decimal pair indexes.

### `ps2-vu-extract`

```text
ps2-vu-extract <packet.bin> <new-private-directory> <unit:0|1>
ps2-vu-extract <program.elf> <new-private-directory> --elf
ps2-vu-extract <program.elf> <new-private-directory> --scan-elf
```

`--elf` reads regions declared by a DVP overlay table. `--scan-elf` finds uploads stored
ready to send, for programs with no such table.

### `ps2-vu-inspect`

```text
ps2-vu-inspect <private-upload.bin> <unit:0|1> <destination-index>
```

### `ps2-vu-recompile`

```text
ps2-vu-recompile <input.bin> <output.ll> <unit:0|1> <destination-index>
    [--all-entries | entry,entry,... [--continuations depth]] [--gpu]
ps2-vu-recompile --catalog <images.tsv> <output.ll> <unit:0|1>
```

`--all-entries` compiles every pair as a possible entry. `--gpu` emits a Vulkan compute
kernel instead of CPU code. `--observe-loads` adds a diagnostic hook on data loads.
A catalog has tab-separated `file` and `destination_pairs` columns, with paths relative
to it. See [batch conversion](batch-conversion.md).

## Research tools

These generate analysis and experimental code. They do not change converted games. See
[loop lifting](render-loop-lifting.md) and [render modernization](render-modernization.md).

### `ps2-vu-loop-probe`

```text
ps2-vu-loop-probe <isolated-loop.bin> <unit> <destination> <manifest|cpu-llvm|spirv-llvm> --native-f32
ps2-vu-loop-probe --structure <program-image.bin> <unit> <destination>
ps2-vu-loop-probe --listing <image> <unit> <destination> <first-pair> <count>
ps2-vu-loop-probe --element <program-image.bin> <unit> <destination> <loop-head> cpu|vulkan|guard
ps2-vu-loop-probe --element-binding <program-image.bin> <unit> <destination> <loop-head> <base-register> <first-row> <tag-low-hex> <registers-hex>
ps2-vu-loop-probe --region <image> <unit> <destination> <head> boundary|elements|binding <entries...>
ps2-vu-loop-probe --regions <whole-entry-image> <unit> <destination> <new-output-directory>
```

### `ps2-vu-render-probe`

```text
ps2-vu-render-probe <private-region.bin> <unit> <destination> <output-vf> <manifest|cpu-llvm|spirv-llvm> --native-f32 [uniform-vf ...]
```

## Small utilities

| Program | Use it to |
| --- | --- |
| `anyps2-display [--smoke]` | Open a window showing a synthetic Vulkan image. `--smoke` is the short test form |
| `anyps2-host-metrics` | List the host's profiling sources, without collecting anything |

## Low-level probes

Generated projects build these into their own executables. Use them only when working
on a converter stage directly.

| Source | Built as | Usage |
| --- | --- | --- |
| `programs/native-probe` | `ps2-native` | `ps2-native [boot.elf] [budget] [--raw] [--capture=NEW_DIRECTORY] [--disc=IMAGE.iso]` |
| `programs/iop-native-probe` | `iop-native` | `iop-native [module.data] [--disc image.iso]` |

Most converted games use the fuller `ps2-native` from `programs/ee-native-probe`
described above.

## Tests

See [testing](testing.md) for choosing and running tests, and
[original PS2 test programs](ps2-test-programs.md) for `tests/ps2_programs/run.py`.
