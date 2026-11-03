# Maintenance boundaries

These are existing code-organization and migration limitations, not a feature
roadmap or a progress log. Resolve them in focused changes with the relevant
behavior tests; remove an entry when its completion condition is met.

| Boundary | Consequence | Completion condition and validation |
| --- | --- | --- |
| [`analysis/src/discover.cpp`](../libraries/analysis/src/discover.cpp) owns instruction discovery, pointer recovery, callback/object analysis and reporting | Changes to independent analyses share a large translation unit and intertwined helpers | Separate whole-code discovery, call/pointer analysis and reports behind explicit conversion-time inputs. Preserve runtime-pointer semantics and conservative data classification; run the affected analysis and callback/whole-code execution regressions |
| [`graphics/src/native_raster.cpp`](../libraries/graphics/src/native_raster.cpp) owns Vulkan resources, pipeline setup, hazards, drawing, texture import and presentation | Resource lifetime and synchronization changes require reviewing unrelated rendering code | Extract cohesive resource/presentation owners only after documenting their fence and invalidation contracts; preserve headless GPU readback tests. Visible comparison remains explicitly requested |
| [`tests/iop_native.cpp`](../tests/iop_native.cpp), [`tests/ee_native.cpp`](../tests/ee_native.cpp) and fixture emitters combine many service/operation families | Large test translation units obscure ownership and require broad recompilation | Split by service or operation family with a shared fixture API, keeping existing milestone test names and actual compiled execution. Do not replace behavior tests with helper-only assertions |
| [`iop_native_modload.hpp`](../libraries/iop/include/nt/iop_native_modload.hpp) and generated EE/IOP bindings retain original-address/register-state adapters | Compiled execution is still not a fully recovered native module ABI | Migrate recoverable calls to symbols/relocations and typed native function pointers without losing genuinely indirect calls. Missing bindings must remain explicit; verify generated execution and dependency audits |
| Root [`CMakeLists.txt`](../CMakeLists.txt) warning flags use a manually maintained target list | New owned targets can miss the common warning policy | Introduce an explicit owned-target helper without applying flags to downloaded dependencies or generated game artifacts; build the affected targets and review new warnings |

Diagnostic `nt_iop`, `nt_hardware`, `nt_kernel` and `nt_runtime` code remains
separate from production generated output. Do not remove it merely because it
resembles a reference model: existing diagnostic tests use it. Do not extend it
as a production fallback either. The
[native dependency guard](../cmake/check_native_output.cmake) checks forbidden
runtime components; passing it does not prove complete instruction semantics.
