cmake_minimum_required(VERSION 3.24)
project(AnyPS2 VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(libraries/graphics/vulkan.cmake)
anyps2_add_vulkan(nt_vulkan)
if(WIN32)
    add_library(nt_host_metrics libraries/host_metrics/src/profile_sources.cpp libraries/host_metrics/src/counter_intervals.cpp libraries/host_metrics/src/etw_counters.cpp)
    target_include_directories(nt_host_metrics PUBLIC libraries/host_metrics/include)
    target_link_libraries(nt_host_metrics PRIVATE advapi32)
    add_executable(anyps2-host-metrics programs/anyps2-host-metrics/main.cpp)
    target_link_libraries(anyps2-host-metrics PRIVATE nt_host_metrics)
    add_executable(anyps2-display programs/anyps2-display/main.cpp)
    target_link_libraries(anyps2-display PRIVATE nt_vulkan user32)
endif()

add_library(nt_binary libraries/binary/src/reader.cpp)
target_include_directories(nt_binary PUBLIC libraries/binary/include)
add_library(nt_disc libraries/disc/src/disc_files.cpp libraries/disc/src/iso9660.cpp libraries/disc/src/reboot_image.cpp)
target_include_directories(nt_disc PUBLIC libraries/disc/include)
target_link_libraries(nt_disc PUBLIC nt_binary)
add_library(nt_elf libraries/elf/src/elf.cpp libraries/elf/src/iop_module.cpp libraries/elf/src/iop_imports.cpp libraries/elf/src/iop_exports.cpp)
target_include_directories(nt_elf PUBLIC libraries/elf/include)
target_link_libraries(nt_elf PUBLIC nt_binary)
add_library(nt_iop
    libraries/iop/src/libraries.cpp libraries/iop/src/native_library.cpp
    libraries/iop/src/dependencies.cpp libraries/iop/src/cpu.cpp
    libraries/iop/src/devices.cpp libraries/iop/src/dma.cpp libraries/iop/src/timers.cpp libraries/iop/src/cdvd.cpp
    libraries/iop/src/intrman.cpp libraries/iop/src/context.cpp libraries/iop/src/interrupts.cpp
    libraries/iop/src/kernel_call.cpp
    libraries/iop/src/dmacman.cpp libraries/iop/src/heaplib.cpp libraries/iop/src/resources.cpp
    libraries/iop/src/registration.cpp libraries/iop/src/startup.cpp libraries/iop/src/reboot.cpp)
target_include_directories(nt_iop PUBLIC libraries/iop/include)
target_link_libraries(nt_iop PUBLIC nt_elf nt_disc)
add_library(nt_iop_native INTERFACE)
target_link_libraries(nt_iop_native INTERFACE nt_binary)
target_include_directories(nt_iop_native INTERFACE libraries/iop/include)
add_library(nt_iop_codegen libraries/iop_codegen/src/ir.cpp libraries/iop_codegen/src/llvm.cpp libraries/iop_codegen/src/project.cpp)
target_include_directories(nt_iop_codegen PUBLIC libraries/iop_codegen/include)
target_link_libraries(nt_iop_codegen PUBLIC nt_elf)
add_executable(ps2-iop-recompile programs/ps2-iop-recompile/main.cpp)
target_link_libraries(ps2-iop-recompile PRIVATE nt_iop_codegen)

add_library(nt_vu libraries/vu/src/upload.cpp libraries/vu/src/program.cpp libraries/vu/src/flow.cpp libraries/vu/src/arithmetic.cpp libraries/vu/src/lower.cpp libraries/vu/src/llvm.cpp)
target_include_directories(nt_vu PUBLIC libraries/vu/include)
target_link_libraries(nt_vu PUBLIC nt_binary)
add_executable(ps2-vu-extract programs/ps2-vu-extract/main.cpp)
target_link_libraries(ps2-vu-extract PRIVATE nt_vu)
add_executable(ps2-vu-inspect programs/ps2-vu-inspect/main.cpp)
target_link_libraries(ps2-vu-inspect PRIVATE nt_vu)
add_executable(ps2-vu-recompile programs/ps2-vu-recompile/main.cpp)
target_link_libraries(ps2-vu-recompile PRIVATE nt_vu)
add_library(nt_r5900 libraries/r5900/src/decoder.cpp)
target_include_directories(nt_r5900 PUBLIC libraries/r5900/include)
add_library(nt_ee_native INTERFACE)
target_link_libraries(nt_ee_native INTERFACE nt_binary)
target_include_directories(nt_ee_native INTERFACE libraries/ee_codegen/include libraries/runtime/include libraries/graphics/include)
add_library(nt_ee_codegen libraries/ee_codegen/src/llvm.cpp libraries/ee_codegen/src/project.cpp)
target_include_directories(nt_ee_codegen PUBLIC libraries/ee_codegen/include)
target_link_libraries(nt_ee_codegen PUBLIC nt_r5900 nt_ee_native)
add_library(nt_analysis libraries/analysis/src/discover.cpp libraries/analysis/src/metrics.cpp)
target_include_directories(nt_analysis PUBLIC libraries/analysis/include)
target_link_libraries(nt_analysis PUBLIC nt_elf nt_r5900)
target_link_libraries(nt_ee_codegen PRIVATE nt_analysis)
add_library(nt_runtime libraries/runtime/src/runtime.cpp)
target_include_directories(nt_runtime PUBLIC libraries/runtime/include)
target_link_libraries(nt_runtime PUBLIC nt_binary)
add_library(nt_fileio libraries/fileio/src/fileio.cpp)
target_include_directories(nt_fileio PUBLIC libraries/fileio/include)
target_link_libraries(nt_fileio PUBLIC nt_disc)
target_include_directories(nt_fileio PUBLIC libraries/runtime/include)
add_library(nt_hardware libraries/hardware/src/system_bus.cpp libraries/hardware/src/graphics_dma.cpp libraries/hardware/src/gs.cpp libraries/hardware/src/gs_vertex.cpp libraries/hardware/src/gs_sprite.cpp libraries/hardware/src/gs_texture.cpp libraries/hardware/src/gif.cpp libraries/hardware/src/vif.cpp libraries/hardware/src/vu_vector.cpp libraries/hardware/src/sif.cpp libraries/hardware/src/sif_commands.cpp libraries/hardware/src/sif_jobs.cpp libraries/hardware/src/sif_rpc.cpp)
target_include_directories(nt_hardware PUBLIC libraries/hardware/include)
target_link_libraries(nt_hardware PUBLIC nt_runtime nt_fileio nt_vulkan nt_iop)
add_library(nt_kernel libraries/kernel/src/kernel.cpp libraries/kernel/src/runner.cpp libraries/kernel/src/interrupts.cpp libraries/kernel/src/interrupt_dispatch.cpp libraries/kernel/src/threads.cpp libraries/kernel/src/sif.cpp)
target_include_directories(nt_kernel PUBLIC libraries/kernel/include)
target_link_libraries(nt_kernel PUBLIC nt_runtime)
add_library(nt_process libraries/process/src/process.cpp)
target_include_directories(nt_process PUBLIC libraries/process/include)

add_executable(ps2-import programs/ps2-import/main.cpp)
target_link_libraries(ps2-import PRIVATE nt_disc nt_elf)
add_executable(ps2-inspect programs/ps2-inspect/main.cpp)
target_link_libraries(ps2-inspect PRIVATE nt_analysis)
add_executable(ps2-recompile programs/ps2-recompile/main.cpp)
target_link_libraries(ps2-recompile PRIVATE nt_ee_codegen)
add_executable(ps2-build programs/ps2-build/main.cpp)
target_link_libraries(ps2-build PRIVATE nt_process)
add_executable(AnyPS2 programs/AnyPS2/main.cpp)
target_link_libraries(AnyPS2 PRIVATE nt_process)
add_dependencies(AnyPS2 ps2-import ps2-inspect ps2-recompile ps2-iop-recompile ps2-build)

include(CTest)
option(ANYPS2_TEST_PRESENTATION "Run a visible Vulkan window integration test" OFF)
if(BUILD_TESTING)
    if(WIN32)
        add_executable(host-metrics-tests tests/host_metrics.cpp)
        target_link_libraries(host-metrics-tests PRIVATE nt_host_metrics)
        add_test(NAME host-metrics-capabilities COMMAND host-metrics-tests)
    endif()
    add_executable(emit-iop-fixture tests/emit_iop_fixture.cpp)
    target_link_libraries(emit-iop-fixture PRIVATE nt_iop_codegen)
    find_program(ANYPS2_CLANG NAMES clang)
    if(ANYPS2_CLANG AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
        add_executable(emit-vu-fixture tests/emit_vu_fixture.cpp)
        target_link_libraries(emit-vu-fixture PRIVATE nt_vu)
        set(vu_fixture_objects)
        set(vu_fixture_commands)
        foreach(fixture IN ITEMS main branch call end_false end_true memory memory1 integer_branch geometry p flags graphics graphics_end graphics_batch continuation clip data shared program program1 q_start q_finish q_early)
            set(source "${CMAKE_CURRENT_BINARY_DIR}/vu-llvm-fixture.ll")
            if(NOT fixture STREQUAL "main")
                string(APPEND source ".${fixture}")
            endif()
            set(object "${CMAKE_CURRENT_BINARY_DIR}/vu-${fixture}.obj")
            list(APPEND vu_fixture_objects "${object}")
            list(APPEND vu_fixture_commands COMMAND "${ANYPS2_CLANG}" -x ir
                -target x86_64-pc-windows-msvc -Werror -Wno-override-module -c -O2 "${source}" -o "${object}")
        endforeach()
        add_custom_command(OUTPUT ${vu_fixture_objects}
            COMMAND emit-vu-fixture "${CMAKE_CURRENT_BINARY_DIR}/vu-llvm-fixture.ll"
            ${vu_fixture_commands}
            DEPENDS emit-vu-fixture VERBATIM)
        add_executable(vu-native-tests tests/vu_native.cpp libraries/ee_native/src/numeric.cpp ${vu_fixture_objects})
        target_include_directories(vu-native-tests PRIVATE libraries/vu/include libraries/runtime/include libraries/ee_codegen/include libraries/graphics/include)
        add_test(NAME vu-llvm-execution COMMAND vu-native-tests)
        add_executable(vu-vulkan-tests tests/vu_vulkan.cpp libraries/ee_native/src/numeric.cpp "${CMAKE_CURRENT_BINARY_DIR}/vu-graphics_end.obj")
        target_include_directories(vu-vulkan-tests PRIVATE libraries/vu/include libraries/runtime/include libraries/ee_codegen/include)
        target_link_libraries(vu-vulkan-tests PRIVATE nt_vulkan)
        add_test(NAME vu-vulkan-rendering COMMAND vu-vulkan-tests)
        set_tests_properties(vu-vulkan-rendering PROPERTIES LABELS gpu TIMEOUT 60)
        add_executable(emit-ee-fixture tests/emit_ee_fixture.cpp)
        target_link_libraries(emit-ee-fixture PRIVATE nt_ee_codegen nt_binary)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj"
            COMMAND emit-ee-fixture "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.ll"
            COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
                "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj"
            DEPENDS emit-ee-fixture VERBATIM)
        add_executable(ee-llvm-tests tests/ee_native.cpp tests/ee_batch.cpp tests/ee_file_rpc.cpp libraries/ee_native/src/io.cpp libraries/ee_native/src/numeric.cpp libraries/ee_native/src/vector.cpp "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj")
        target_sources(vu-native-tests PRIVATE libraries/ee_native/src/io.cpp libraries/ee_native/src/vector.cpp "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj")
        target_link_libraries(ee-llvm-tests PRIVATE nt_ee_native nt_fileio)
        add_test(NAME ee-llvm-execution COMMAND ee-llvm-tests)
        add_test(NAME ee-llvm-project COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/ee-project-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_project.cmake")
        set_tests_properties(ee-llvm-project PROPERTIES TIMEOUT 120)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj"
            COMMAND emit-iop-fixture "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-reference.ll"
                "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.irx" "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.ll"
            COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
                "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj"
            DEPENDS emit-iop-fixture VERBATIM)
        add_executable(iop-llvm-tests tests/iop_native.cpp tests/iop_llvm_adapter.cpp "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj")
        target_link_libraries(iop-llvm-tests PRIVATE nt_iop_native nt_disc)
        target_compile_definitions(iop-llvm-tests PRIVATE ANYPS2_TEST_LARGE_LLVM=1)
        add_test(NAME iop-llvm-execution COMMAND iop-llvm-tests)
        add_executable(sif-native-tests tests/sif_native.cpp
            libraries/ee_native/src/io.cpp libraries/ee_native/src/numeric.cpp libraries/ee_native/src/vector.cpp
            "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj" "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj")
        target_link_libraries(sif-native-tests PRIVATE nt_ee_native nt_iop_native nt_fileio)
        add_test(NAME native-sif-link COMMAND sif-native-tests)
        add_test(NAME iop-llvm-project COMMAND "${CMAKE_COMMAND}"
            "-DEEFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DEECONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DFIXTURE=$<TARGET_FILE:emit-iop-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-iop-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/iop-project-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/iop_project.cmake")
        set_tests_properties(iop-llvm-project PROPERTIES TIMEOUT 120)
    endif()
    add_executable(vulkan-tests tests/vulkan.cpp)
    target_link_libraries(vulkan-tests PRIVATE nt_vulkan)
    add_test(NAME vulkan-rendering COMMAND vulkan-tests)
    set_tests_properties(vulkan-rendering PROPERTIES LABELS "gpu" TIMEOUT 60)
    if(WIN32 AND ANYPS2_TEST_PRESENTATION)
        add_test(NAME vulkan-presentation COMMAND anyps2-display --smoke)
        set_tests_properties(vulkan-presentation PROPERTIES LABELS "gpu;interactive" TIMEOUT 60)
    endif()
    add_executable(format-tests tests/formats.cpp)
    target_link_libraries(format-tests PRIVATE nt_disc nt_elf nt_iop)
    add_test(NAME formats COMMAND format-tests)
    add_executable(iop-execution-tests tests/iop_execution.cpp)
    target_link_libraries(iop-execution-tests PRIVATE nt_iop)
    add_test(NAME iop-execution COMMAND iop-execution-tests)
    add_executable(analysis-tests tests/analysis.cpp)
    target_link_libraries(analysis-tests PRIVATE nt_analysis)
    add_test(NAME analysis COMMAND analysis-tests)
    add_executable(process-tests tests/process.cpp)
    target_link_libraries(process-tests PRIVATE nt_process)
    add_test(NAME process-arguments COMMAND process-tests)
    add_executable(kernel-tests tests/kernel.cpp)
    target_link_libraries(kernel-tests PRIVATE nt_kernel)
    add_test(NAME bios-free-kernel COMMAND kernel-tests)
    add_executable(vu-program-tests tests/vu_program.cpp)
    target_link_libraries(vu-program-tests PRIVATE nt_vu)
    add_test(NAME vu-program-framing COMMAND vu-program-tests)
    add_executable(thread-tests tests/threads.cpp)
    target_link_libraries(thread-tests PRIVATE nt_kernel)
    add_test(NAME bios-free-threads COMMAND thread-tests)
    add_executable(hardware-tests tests/hardware.cpp)
    target_link_libraries(hardware-tests PRIVATE nt_hardware nt_kernel)
    add_test(NAME hardware-registers COMMAND hardware-tests)
    add_executable(sif-tests tests/sif.cpp)
    target_link_libraries(sif-tests PRIVATE nt_hardware nt_kernel)
    add_test(NAME sif-transport COMMAND sif-tests)
    add_executable(fileio-tests tests/fileio.cpp)
    target_link_libraries(fileio-tests PRIVATE nt_hardware)
    add_test(NAME disc-fileio-rpc COMMAND fileio-tests)
    add_test(NAME repository-content COMMAND "${CMAKE_COMMAND}"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/repository-policy"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/repository_content.cmake")
    add_test(NAME repository-policy-tests COMMAND "${CMAKE_COMMAND}"
        "-DPOLICY=${CMAKE_CURRENT_SOURCE_DIR}/tests/repository_content.cmake"
        "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/policy-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/repository_policy_tests.cmake")
    set(ANYPS2_TEST_PROBE "" CACHE FILEPATH "Optional local generated startup executable")
    set(ANYPS2_TEST_IMAGE "" CACHE FILEPATH "Optional matching local boot ELF")
    set(ANYPS2_TEST_DISC "" CACHE FILEPATH "Optional read-only disc image for local FILEIO")
    set(ANYPS2_EXPECTED_STOP "" CACHE STRING "Expected startup diagnostic regex")
    set(ANYPS2_TEST_BUDGET "1000000" CACHE STRING "Instruction limit for local startup regression")
    set(ANYPS2_EXPECTED_EXIT "3" CACHE STRING "Local test exit: 3 diagnostic stop, 4 explicit budget boundary")
    if(ANYPS2_TEST_PROBE)
        add_test(NAME local-startup COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=${ANYPS2_TEST_PROBE}" "-DIMAGE=${ANYPS2_TEST_IMAGE}"
            "-DDISC=${ANYPS2_TEST_DISC}"
            "-DEXPECTED=${ANYPS2_EXPECTED_STOP}"
            "-DBUDGET=${ANYPS2_TEST_BUDGET}"
            "-DEXPECTED_EXIT=${ANYPS2_EXPECTED_EXIT}"
            "-DLOG=${CMAKE_CURRENT_BINARY_DIR}/local-startup.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_startup.cmake")
        set_tests_properties(local-startup PROPERTIES LABELS "local;integration" TIMEOUT 70)
    endif()
endif()
foreach(target nt_vu ps2-vu-inspect ps2-vu-recompile nt_ee_codegen nt_binary nt_disc nt_elf nt_iop nt_iop_codegen nt_r5900 nt_analysis nt_runtime nt_fileio nt_hardware nt_kernel nt_process ps2-import ps2-inspect ps2-recompile ps2-iop-recompile ps2-build AnyPS2)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
