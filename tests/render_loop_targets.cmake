add_executable(render-loop-tests tests/render_loop.cpp)
target_link_libraries(render-loop-tests PRIVATE nt_render_codegen)
add_test(NAME render-loop-lifting COMMAND render-loop-tests)
add_executable(render-loop-structure-tests tests/loop_structure.cpp)
target_link_libraries(render-loop-structure-tests PRIVATE nt_render_codegen)
add_test(NAME render-loop-structure COMMAND render-loop-structure-tests)
# Recovered element kernel of an original synthetic quotient loop, compiled from
# emitted LLVM and executed on the CPU and, when enabled, on a Vulkan device.
set(element_prefix "${CMAKE_CURRENT_BINARY_DIR}/render-element")
add_custom_command(OUTPUT "${element_prefix}.cpu.ll" "${element_prefix}.gpu.ll"
    COMMAND render-loop-structure-tests --emit-element quotient "${element_prefix}"
    DEPENDS render-loop-structure-tests VERBATIM)
add_custom_target(render-element-fixture ALL DEPENDS "${element_prefix}.cpu.ll" "${element_prefix}.gpu.ll")
if(ANYPS2_RENDER_CLANG)
    if(WIN32)
        set(element_object "${element_prefix}.obj")
    else()
        set(element_object "${element_prefix}.o")
    endif()
    add_custom_command(OUTPUT "${element_object}"
        COMMAND "${ANYPS2_RENDER_CLANG}" -x ir -O2 -Werror -Wno-override-module -c
            "${element_prefix}.cpu.ll" -o "${element_object}"
        DEPENDS render-element-fixture "${element_prefix}.cpu.ll" VERBATIM)
    set_source_files_properties("${element_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_executable(render-element-native-tests tests/element_kernel_native.cpp
        libraries/ee_native/src/gpu_numeric.cpp "${element_object}")
    target_include_directories(render-element-native-tests PRIVATE libraries/runtime/include)
    target_link_libraries(render-element-native-tests PRIVATE nt_render_codegen)
    add_test(NAME render-element-llvm-execution COMMAND render-element-native-tests)
    if(ANYPS2_TEST_RENDER_SPIRV OR ANYPS2_TEST_LLVM_GPU)
        include(cmake/llvm_shaders.cmake)
        anyps2_compile_llvm_shader(element_shader "${element_prefix}.gpu.ll" STRUCTURIZE)
        add_custom_target(render-element-spirv ALL DEPENDS "${element_shader}")
        add_dependencies(render-element-spirv render-element-fixture)
        add_test(NAME render-element-spirv-validation
            COMMAND "${ANYPS2_SPIRV_VAL}" --target-env vulkan1.3 "${element_shader}")
    endif()
    if(ANYPS2_TEST_LLVM_GPU)
        find_package(Vulkan REQUIRED)
        add_executable(render-element-vulkan-tests tests/element_kernel_native.cpp
            libraries/ee_native/src/gpu_numeric.cpp "${element_object}")
        target_include_directories(render-element-vulkan-tests PRIVATE libraries/runtime/include)
        target_compile_definitions(render-element-vulkan-tests PRIVATE ANYPS2_ELEMENT_VULKAN)
        target_link_libraries(render-element-vulkan-tests PRIVATE nt_render_codegen Vulkan::Vulkan)
        add_dependencies(render-element-vulkan-tests render-element-spirv)
        add_test(NAME render-element-vulkan-execution COMMAND render-element-vulkan-tests "${element_shader}")
        set_tests_properties(render-element-vulkan-execution PROPERTIES LABELS "gpu" TIMEOUT 60)
        if(TARGET nt_vulkan)
            set(vertex_prefix "${CMAKE_CURRENT_BINARY_DIR}/render-vertex")
            add_custom_command(OUTPUT "${vertex_prefix}.cpu.ll" "${vertex_prefix}.gpu.ll"
                COMMAND render-loop-structure-tests --emit-element vertex "${vertex_prefix}"
                DEPENDS render-loop-structure-tests VERBATIM)
            add_custom_target(render-vertex-fixture DEPENDS "${vertex_prefix}.cpu.ll" "${vertex_prefix}.gpu.ll")
            if(WIN32)
                set(vertex_object "${vertex_prefix}.obj")
            else()
                set(vertex_object "${vertex_prefix}.o")
            endif()
            add_custom_command(OUTPUT "${vertex_object}"
                COMMAND "${ANYPS2_RENDER_CLANG}" -x ir -O2 -Werror -Wno-override-module -c
                    "${vertex_prefix}.cpu.ll" -o "${vertex_object}"
                DEPENDS render-vertex-fixture "${vertex_prefix}.cpu.ll" VERBATIM)
            set_source_files_properties("${vertex_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
            anyps2_compile_llvm_shader(vertex_shader "${vertex_prefix}.gpu.ll" STRUCTURIZE)
            add_custom_target(render-vertex-spirv DEPENDS "${vertex_shader}")
            add_dependencies(render-vertex-spirv render-vertex-fixture)
            add_executable(render-vector-draw-tests tests/vector_draw.cpp
                libraries/ee_native/src/gpu_numeric.cpp "${vertex_object}")
            target_include_directories(render-vector-draw-tests PRIVATE libraries/runtime/include)
            target_link_libraries(render-vector-draw-tests PRIVATE nt_render_codegen nt_vulkan)
            add_dependencies(render-vector-draw-tests render-vertex-spirv render-element-spirv)
            add_test(NAME render-vector-drawing COMMAND render-vector-draw-tests "${vertex_shader}" "${element_shader}")
            set_tests_properties(render-vector-drawing PROPERTIES LABELS "gpu" TIMEOUT 120)
        endif()
    endif()
endif()
foreach(variant 0 1)
    set(prefix "${CMAKE_CURRENT_BINARY_DIR}/render-loop-${variant}")
    add_custom_command(OUTPUT "${prefix}.cpu.ll" "${prefix}.gpu.ll" "${prefix}.json" "${prefix}.bin"
        COMMAND render-loop-tests --emit "${prefix}" "${variant}"
        DEPENDS render-loop-tests VERBATIM)
    add_custom_target(render-loop-fixture-${variant} ALL
        DEPENDS "${prefix}.cpu.ll" "${prefix}.gpu.ll" "${prefix}.json" "${prefix}.bin")
    if(variant EQUAL 0)
        set(destination 0)
    else()
        set(destination 112)
    endif()
    add_test(NAME render-loop-probe-${variant} COMMAND "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:ps2-vu-loop-probe>" "-DPREFIX=${prefix}" "-DDESTINATION=${destination}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_loop_probe.cmake")
    if(ANYPS2_RENDER_CLANG)
        if(WIN32)
            set(object "${prefix}.obj")
        else()
            set(object "${prefix}.o")
        endif()
        add_custom_command(OUTPUT "${object}"
            COMMAND "${ANYPS2_RENDER_CLANG}" -x ir -O3 -Werror -Wno-override-module -c
                "${prefix}.cpu.ll" -o "${object}"
            DEPENDS render-loop-fixture-${variant} "${prefix}.cpu.ll" VERBATIM)
        set_source_files_properties("${object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        add_executable(render-loop-native-${variant} tests/render_loop_native.cpp "${object}")
        target_link_libraries(render-loop-native-${variant} PRIVATE nt_render_codegen)
        target_compile_definitions(render-loop-native-${variant} PRIVATE ANYPS2_LOOP_VARIANT=${variant})
        if(MSVC)
            target_compile_options(render-loop-native-${variant} PRIVATE /fp:strict)
        else()
            target_compile_options(render-loop-native-${variant} PRIVATE -ffp-contract=off)
        endif()
        add_test(NAME render-loop-llvm-execution-${variant} COMMAND render-loop-native-${variant})
    endif()
    if(ANYPS2_TEST_RENDER_SPIRV)
        anyps2_compile_llvm_shader(loop_shader "${prefix}.gpu.ll")
        add_custom_target(render-loop-spirv-${variant} ALL DEPENDS "${loop_shader}")
        add_dependencies(render-loop-spirv-${variant} render-loop-fixture-${variant})
        add_test(NAME render-loop-spirv-validation-${variant}
            COMMAND "${ANYPS2_SPIRV_VAL}" --target-env vulkan1.3 "${loop_shader}")
    endif()
endforeach()
