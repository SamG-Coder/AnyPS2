# Pure conversion-time research tools. These targets do not change generated
# games or install an alternative renderer.
add_library(nt_render_codegen
    libraries/render_codegen/src/vu_lift.cpp
    libraries/render_codegen/src/llvm.cpp
    libraries/render_codegen/src/vertex_loop.cpp
    libraries/render_codegen/src/loop_structure.cpp
    libraries/render_codegen/src/loop_pipeline.cpp
    libraries/render_codegen/src/element_llvm.cpp
    libraries/render_codegen/src/vertex_loop_llvm.cpp)
target_include_directories(nt_render_codegen PUBLIC libraries/render_codegen/include)
target_link_libraries(nt_render_codegen PUBLIC nt_vu nt_gpu_codegen)
target_compile_features(nt_render_codegen PUBLIC cxx_std_20)
add_executable(ps2-vu-render-probe programs/ps2-vu-render-probe/main.cpp)
target_link_libraries(ps2-vu-render-probe PRIVATE nt_render_codegen)
add_executable(ps2-vu-loop-probe programs/ps2-vu-loop-probe/main.cpp)
target_link_libraries(ps2-vu-loop-probe PRIVATE nt_render_codegen)
foreach(target nt_render_codegen ps2-vu-render-probe ps2-vu-loop-probe)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
