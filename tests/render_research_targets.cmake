option(ANYPS2_TEST_RENDER_SPIRV "Compile and validate research render SPIR-V (no GPU execution)" OFF)
if(NOT BUILD_TESTING)
    return()
endif()
add_executable(render-lift-tests tests/render_lift.cpp)
target_link_libraries(render-lift-tests PRIVATE nt_render_codegen)
add_test(NAME render-kernel-lifting COMMAND render-lift-tests)
set(render_fixture "${CMAKE_CURRENT_BINARY_DIR}/render-kernel-fixture")
add_custom_command(OUTPUT "${render_fixture}.cpu.ll" "${render_fixture}.gpu.ll" "${render_fixture}.json" "${render_fixture}.bin"
    COMMAND render-lift-tests --emit "${render_fixture}"
    DEPENDS render-lift-tests VERBATIM)
add_custom_target(render-research-fixtures ALL
    DEPENDS "${render_fixture}.cpu.ll" "${render_fixture}.gpu.ll" "${render_fixture}.json" "${render_fixture}.bin")
add_test(NAME render-kernel-probe COMMAND "${CMAKE_COMMAND}"
    "-DPROBE=$<TARGET_FILE:ps2-vu-render-probe>" "-DPREFIX=${render_fixture}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_probe.cmake")
find_program(ANYPS2_RENDER_CLANG NAMES clang)
if(ANYPS2_RENDER_CLANG)
    if(WIN32)
        set(render_object "${CMAKE_CURRENT_BINARY_DIR}/render-kernel-fixture.obj")
    else()
        set(render_object "${CMAKE_CURRENT_BINARY_DIR}/render-kernel-fixture.o")
    endif()
    add_custom_command(OUTPUT "${render_object}"
        COMMAND "${ANYPS2_RENDER_CLANG}" -x ir -O2 -Werror -Wno-override-module -c
            "${render_fixture}.cpu.ll" -o "${render_object}"
        DEPENDS render-research-fixtures "${render_fixture}.cpu.ll" VERBATIM)
    set_source_files_properties("${render_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_executable(render-native-tests tests/render_native.cpp "${render_object}")
    if(MSVC)
        target_compile_options(render-native-tests PRIVATE /fp:strict)
    else()
        target_compile_options(render-native-tests PRIVATE -ffp-contract=off)
    endif()
    add_test(NAME render-kernel-llvm-execution COMMAND render-native-tests)
else()
    message(STATUS "clang not found: compiled render-kernel execution test is unavailable")
endif()
if(ANYPS2_TEST_RENDER_SPIRV)
    include(cmake/llvm_shaders.cmake)
    anyps2_compile_llvm_shader(render_shader "${render_fixture}.gpu.ll")
    add_custom_target(render-research-spirv ALL DEPENDS "${render_shader}")
    add_dependencies(render-research-spirv render-research-fixtures)
    add_test(NAME render-kernel-spirv-validation
        COMMAND "${ANYPS2_SPIRV_VAL}" --target-env vulkan1.3 "${render_shader}")
endif()
