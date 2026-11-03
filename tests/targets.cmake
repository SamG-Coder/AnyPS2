include(cmake/llvm_shaders.cmake)
option(ANYPS2_TEST_LLVM_GPU "Compile and execute LLVM-generated Vulkan kernels" OFF)
if(BUILD_TESTING)
    add_executable(emit-gpu-fixture tests/emit_gpu_fixture.cpp)
    target_link_libraries(emit-gpu-fixture PRIVATE nt_gpu_codegen)
endif()
if(BUILD_TESTING AND ANYPS2_TEST_LLVM_GPU)
    set(gpu_fixture "${CMAKE_CURRENT_BINARY_DIR}/gpu-fixture")
    add_custom_command(OUTPUT "${gpu_fixture}.ll" "${gpu_fixture}-cpu.ll"
        COMMAND emit-gpu-fixture "${gpu_fixture}.ll" "${gpu_fixture}-cpu.ll"
        DEPENDS emit-gpu-fixture VERBATIM)
    anyps2_compile_llvm_shader(gpu_shader "${gpu_fixture}.ll" STRUCTURIZE)
    add_custom_command(OUTPUT "${gpu_fixture}-cpu.obj"
        COMMAND "${ANYPS2_GPU_CLANG}" -x ir -target x86_64-pc-windows-msvc -O2 -c
            "${gpu_fixture}-cpu.ll" -o "${gpu_fixture}-cpu.obj"
        DEPENDS "${gpu_fixture}-cpu.ll" VERBATIM)
    add_executable(gpu-kernel-tests tests/gpu_kernel.cpp libraries/ee_native/src/gpu_numeric.cpp
        "${gpu_fixture}-cpu.obj" "${gpu_shader}")
    target_include_directories(gpu-kernel-tests PRIVATE libraries/runtime/include)
    target_link_libraries(gpu-kernel-tests PRIVATE nt_gpu_codegen Vulkan::Vulkan)
    add_test(NAME llvm-gpu-kernel COMMAND gpu-kernel-tests "${gpu_shader}")
    set_tests_properties(llvm-gpu-kernel PROPERTIES LABELS "gpu" TIMEOUT 60)
endif()
if(BUILD_TESTING)
    add_executable(pad-input-tests tests/pad_input.cpp)
    target_link_libraries(pad-input-tests PRIVATE nt_input nt_binary)
    target_include_directories(pad-input-tests PRIVATE libraries/ee_codegen/include libraries/runtime/include)
    add_test(NAME pad-input COMMAND pad-input-tests)
    add_executable(sample-audio-tests tests/sample_audio.cpp)
    target_link_libraries(sample-audio-tests PRIVATE nt_audio nt_binary)
    target_include_directories(sample-audio-tests PRIVATE libraries/ee_codegen/include libraries/runtime/include)
    add_test(NAME sample-audio COMMAND sample-audio-tests)
    add_executable(gamepad-host-tests tests/gamepad_host.cpp)
    target_link_libraries(gamepad-host-tests PRIVATE nt_input SDL3::SDL3-static)
    add_test(NAME gamepad-host COMMAND gamepad-host-tests)
    set_tests_properties(gamepad-host PROPERTIES ENVIRONMENT
        "SDL_JOYSTICK_HIDAPI=0;SDL_JOYSTICK_RAWINPUT=0;SDL_JOYSTICK_DIRECTINPUT=0;SDL_XINPUT_ENABLED=0;SDL_JOYSTICK_WGI=0;SDL_JOYSTICK_GAMEINPUT=0")
    add_executable(iop-inspection-tests tests/iop_inspection.cpp)
    target_link_libraries(iop-inspection-tests PRIVATE nt_inventory)
    add_test(NAME iop-neighborhood-inspection COMMAND iop-inspection-tests)
endif()
option(ANYPS2_TEST_PRESENTATION "Run a visible Vulkan window integration test" OFF)
option(ANYPS2_TEST_USB_DEVICES "Query real Windows USB descriptors" OFF)
option(ANYPS2_TEST_AUDIO_DEVICE "Run initialization against the Windows audio output device" OFF)
if(BUILD_TESTING)
    add_executable(packed-image-tests tests/packed_image.cpp)
    target_include_directories(packed-image-tests PRIVATE "${anyps2_zlib_SOURCE_DIR}" "${anyps2_zlib_BINARY_DIR}")
    target_link_libraries(packed-image-tests PRIVATE nt_package nt_elf zlibstatic)
    add_test(NAME packed-image COMMAND packed-image-tests)
    if(WIN32)
        add_executable(usb-host-tests tests/usb_host.cpp)
        target_link_libraries(usb-host-tests PRIVATE nt_usb)
        add_test(NAME usb-descriptors COMMAND usb-host-tests)
        if(ANYPS2_TEST_USB_DEVICES)
            add_test(NAME native-usb-devices COMMAND usb-host-tests --devices)
        endif()
        add_executable(audio-output-tests tests/audio_output.cpp)
        target_link_libraries(audio-output-tests PRIVATE nt_audio)
        if(ANYPS2_TEST_AUDIO_DEVICE)
            add_test(NAME native-audio-device COMMAND audio-output-tests)
        endif()
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
        include(libraries/ee_native/numeric.cmake)
        anyps2_numeric_bitcode(vu_numeric_bitcode)
        foreach(fixture IN ITEMS main range branch call end_false end_true memory memory1 integer_branch geometry p flags mac_pipeline graphics graphics_end graphics_batch continuation whole clip data shared program program1 q_start q_finish q_early q_elapsed q_scheduled)
            set(source "${CMAKE_CURRENT_BINARY_DIR}/vu-llvm-fixture.ll")
            if(NOT fixture STREQUAL "main")
                string(APPEND source ".${fixture}")
            endif()
            set(object "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vu-${fixture}.obj")
            list(APPEND vu_fixture_objects "${object}")
            list(APPEND vu_fixture_commands COMMAND "${ANYPS2_CLANG}" -x ir
                -target x86_64-pc-windows-msvc -Werror -Wno-override-module -c -O2 "${source}"
                -Xclang -mlink-builtin-bitcode -Xclang "${vu_numeric_bitcode}" -o "${object}")
        endforeach()
        add_custom_command(OUTPUT ${vu_fixture_objects}
            COMMAND emit-vu-fixture "${CMAKE_CURRENT_BINARY_DIR}/vu-llvm-fixture.ll"
            ${vu_fixture_commands}
            DEPENDS emit-vu-fixture "${vu_numeric_bitcode}" VERBATIM)
        add_executable(vu-native-tests tests/vu_native.cpp libraries/ee_native/src/numeric.cpp ${vu_fixture_objects})
        target_include_directories(vu-native-tests PRIVATE libraries/vu/include libraries/runtime/include libraries/ee_codegen/include libraries/graphics/include)
        add_test(NAME vu-llvm-execution COMMAND vu-native-tests)
        add_executable(vu-vulkan-tests tests/vu_vulkan.cpp libraries/ee_native/src/numeric.cpp "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vu-graphics_end.obj")
        target_include_directories(vu-vulkan-tests PRIVATE libraries/vu/include libraries/runtime/include libraries/ee_codegen/include)
        target_link_libraries(vu-vulkan-tests PRIVATE nt_vulkan)
        add_test(NAME vu-vulkan-rendering COMMAND vu-vulkan-tests)
        set_tests_properties(vu-vulkan-rendering PROPERTIES LABELS gpu TIMEOUT 60)
        add_executable(emit-ee-fixture tests/emit_ee_fixture.cpp)
        target_link_libraries(emit-ee-fixture PRIVATE nt_ee_codegen nt_binary nt_analysis)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj"
            COMMAND emit-ee-fixture "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.ll"
            COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
                "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj"
            DEPENDS emit-ee-fixture VERBATIM)
        # Several native test programs share this object. Give generation one
        # owner so parallel Visual Studio builds cannot truncate each other's IR.
        add_custom_target(ee-fixture-object DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj")
        add_executable(ee-llvm-tests tests/ee_native.cpp tests/ee_batch.cpp tests/ee_file_rpc.cpp tests/ee_scratchpad.cpp libraries/ee_native/src/io.cpp libraries/ee_native/src/numeric.cpp libraries/ee_native/src/vector.cpp "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj")
        target_sources(vu-native-tests PRIVATE libraries/ee_native/src/io.cpp libraries/ee_native/src/vector.cpp "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj")
        target_link_libraries(ee-llvm-tests PRIVATE nt_ee_native nt_fileio)
        add_test(NAME ee-llvm-execution COMMAND ee-llvm-tests)
        add_executable(emit-whole-code-fixture tests/emit_whole_code_fixture.cpp)
        target_link_libraries(emit-whole-code-fixture PRIVATE nt_ee_codegen nt_analysis)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.obj"
            COMMAND emit-whole-code-fixture "${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.ll"
            COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
                "${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.obj"
            DEPENDS emit-whole-code-fixture VERBATIM)
        add_executable(ee-whole-code-tests tests/whole_code_native.cpp
            libraries/ee_native/src/io.cpp libraries/ee_native/src/numeric.cpp libraries/ee_native/src/vector.cpp
            "${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.obj")
        target_link_libraries(ee-whole-code-tests PRIVATE nt_ee_native)
        add_test(NAME ee-whole-code-execution COMMAND ee-whole-code-tests)
        add_test(NAME ee-whole-code-project COMMAND "${CMAKE_COMMAND}"
            "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.ll.elf"
            "-DUNSUPPORTED=${CMAKE_CURRENT_BINARY_DIR}/whole-code-fixture.ll.unsupported.elf"
            "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/whole-code-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/whole_code_project.cmake")
        add_test(NAME ee-llvm-project COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/ee-project-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_project.cmake")
        # Nine independent Release builds share this gate; each build command
        # retains its own 90-second limit in ee_project.cmake.
        set_tests_properties(ee-llvm-project PROPERTIES TIMEOUT 600)
        add_test(NAME ee-vector-container COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/vector-section-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/vector_section_project.cmake")
        set_tests_properties(ee-vector-container PROPERTIES TIMEOUT 120)
        add_test(NAME ee-pointer-flow COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/pointer-flow-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/pointer_flow_project.cmake")
        set_tests_properties(ee-pointer-flow PROPERTIES TIMEOUT 900)
        add_test(NAME ee-object-callbacks COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/object-callback-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/object_callback_project.cmake")
        set_tests_properties(ee-object-callbacks PROPERTIES TIMEOUT 240)
        add_test(NAME ee-flow-verification COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/flow-verification-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/flow_verification_project.cmake")
        set_tests_properties(ee-flow-verification PROPERTIES TIMEOUT 90)
        add_executable(ee-observation-tests tests/flow_observation.cpp)
        target_link_libraries(ee-observation-tests PRIVATE nt_ee_codegen)
        add_test(NAME ee-flow-observations COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DCHECKER=$<TARGET_FILE:ee-observation-tests>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/observation-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/flow_observation_project.cmake")
        set_tests_properties(ee-flow-observations PROPERTIES TIMEOUT 180)
        set(ANYPS2_TEST_PS2DEV "" CACHE PATH "Optional PS2DEV toolchain for original C++ PS2 tests")
        set(ANYPS2_PS2_HOST_RUNTIME "" CACHE PATH "Optional MinGW32 DLL directory for the PS2 cross-compiler")
        if(ANYPS2_TEST_PS2DEV)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(ps2_host_arguments)
            if(ANYPS2_PS2_HOST_RUNTIME)
                list(APPEND ps2_host_arguments --host-runtime "${ANYPS2_PS2_HOST_RUNTIME}")
            endif()
            add_test(NAME ps2-source-protocol COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ps2_programs/test_runner.py")
            # The full corpus is an explicit gap audit. Callback and virtual-object
            # cases currently expose missing discovery and must not count as passes.
            function(add_ps2_source_test name)
                add_test(NAME ps2-source-${name} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/ps2_programs/run.py"
                    --ps2dev "${ANYPS2_TEST_PS2DEV}" ${ps2_host_arguments}
                    --converter "$<TARGET_FILE:ps2-recompile>" --generator "${CMAKE_GENERATOR}"
                    --output-root "${CMAKE_CURRENT_BINARY_DIR}/ps2-source-tests" ${ARGN})
                set_tests_properties(ps2-source-${name} PROPERTIES TIMEOUT 300)
            endfunction()
            add_ps2_source_test(scalar --case scalar)
            add_ps2_source_test(direct_flow --case direct_flow)
            add_ps2_source_test(scalar-size --case scalar --profile Os)
            add_ps2_source_test(weak-callback --case weak_callback --profile O2)
            add_ps2_source_test(switch-spill --case switch_spill --profile O2)
            add_ps2_source_test(function-symbols --case callback_stack --profile O2 --function-symbols)
        endif()
        add_test(NAME ee-native-images COMMAND "${CMAKE_COMMAND}"
            -DFIXTURE=$<TARGET_FILE:emit-ee-fixture> -DCONVERTER=$<TARGET_FILE:ps2-recompile>
            -DSCRATCH=${CMAKE_BINARY_DIR}/image-project-tests -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DGENERATOR=${CMAKE_GENERATOR} -DPLATFORM=${CMAKE_GENERATOR_PLATFORM} -DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/images_project.cmake")
        set_tests_properties(ee-native-images PROPERTIES TIMEOUT 300)
        add_test(NAME packed-llvm-project COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:packed-image-tests>" "-DUNPACKER=$<TARGET_FILE:ps2-unpack>"
            "-DCONVERTER=$<TARGET_FILE:ps2-recompile>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/packed-project-tests" "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/packed_project.cmake")
        set_tests_properties(packed-llvm-project PROPERTIES TIMEOUT 120)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj"
            COMMAND emit-iop-fixture "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-reference.ll"
                "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.irx" "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.ll"
            COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
                "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj"
            DEPENDS emit-iop-fixture VERBATIM)
        add_executable(iop-llvm-tests tests/iop_native.cpp tests/iop_llvm_adapter.cpp "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj")
        target_link_libraries(iop-llvm-tests PRIVATE nt_iop_native nt_iop nt_disc)
        target_compile_definitions(iop-llvm-tests PRIVATE ANYPS2_TEST_LARGE_LLVM=1)
        add_test(NAME iop-llvm-execution COMMAND iop-llvm-tests)
        add_executable(sif-native-tests tests/sif_native.cpp tests/iop_reset.cpp
            libraries/ee_native/src/io.cpp libraries/ee_native/src/numeric.cpp libraries/ee_native/src/vector.cpp
            "${CMAKE_CURRENT_BINARY_DIR}/ee-llvm-fixture.obj" "${CMAKE_CURRENT_BINARY_DIR}/iop-llvm-fixture.obj")
        target_link_libraries(sif-native-tests PRIVATE nt_ee_native nt_iop_native nt_fileio)
        add_test(NAME native-sif-link COMMAND sif-native-tests)
        add_dependencies(ee-llvm-tests ee-fixture-object)
        add_dependencies(vu-native-tests ee-fixture-object)
        add_dependencies(sif-native-tests ee-fixture-object)
        add_test(NAME iop-llvm-project COMMAND "${CMAKE_COMMAND}"
            "-DEEFIXTURE=$<TARGET_FILE:emit-ee-fixture>" "-DEECONVERTER=$<TARGET_FILE:ps2-recompile>"
            "-DFIXTURE=$<TARGET_FILE:emit-iop-fixture>" "-DCONVERTER=$<TARGET_FILE:ps2-iop-recompile>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/iop-project-tests"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/iop_project.cmake")
        set_tests_properties(iop-llvm-project PROPERTIES TIMEOUT 120)
    endif()
    add_executable(native-raster-tests tests/native_raster.cpp)
    target_link_libraries(native-raster-tests PRIVATE nt_vulkan)
    add_test(NAME native-vulkan-raster COMMAND native-raster-tests)
    set_tests_properties(native-vulkan-raster PROPERTIES LABELS "gpu" TIMEOUT 120)
    if(ANYPS2_LLVM_TEXTURES)
        add_test(NAME native-vulkan-llvm-textures COMMAND native-raster-tests --require-llvm-textures)
        set_tests_properties(native-vulkan-llvm-textures PROPERTIES LABELS "gpu" TIMEOUT 120)
    endif()
    add_executable(vulkan-tests tests/vulkan.cpp)
    target_link_libraries(vulkan-tests PRIVATE nt_vulkan)
    add_test(NAME vulkan-rendering COMMAND vulkan-tests)
    add_test(NAME vulkan-resident-rendering COMMAND vulkan-tests --resident)
    set_tests_properties(vulkan-resident-rendering PROPERTIES LABELS "gpu" TIMEOUT 60)
    set_tests_properties(vulkan-rendering PROPERTIES LABELS "gpu" TIMEOUT 60)
    if(WIN32 AND ANYPS2_TEST_PRESENTATION)
        add_executable(native-presentation-tests tests/native_presentation.cpp)
        target_link_libraries(native-presentation-tests PRIVATE nt_vulkan)
        add_test(NAME native-vulkan-presentation COMMAND native-presentation-tests)
        set_tests_properties(native-vulkan-presentation PROPERTIES LABELS "gpu;interactive" TIMEOUT 60
            ENVIRONMENT "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
        add_test(NAME vulkan-presentation COMMAND anyps2-display --smoke)
        set_tests_properties(vulkan-presentation PROPERTIES LABELS "gpu;interactive" TIMEOUT 60)
    endif()
    add_executable(format-tests tests/formats.cpp)
    target_link_libraries(format-tests PRIVATE nt_disc nt_elf nt_iop)
    add_test(NAME formats COMMAND format-tests)
    add_executable(inventory-tests tests/inventory.cpp)
    target_link_libraries(inventory-tests PRIVATE nt_inventory)
    add_test(NAME conversion-inventory COMMAND inventory-tests)
    if(ANYPS2_CLANG)
        target_compile_definitions(inventory-tests PRIVATE ANYPS2_TEST_CLANG="${ANYPS2_CLANG}")
        add_test(NAME conversion-auto-project COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE=$<TARGET_FILE:inventory-tests>" "-DCONVERTER=$<TARGET_FILE:AnyPS2>"
            "-DPACKED_FIXTURE=$<TARGET_FILE:packed-image-tests>"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/inventory-project-tests"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/inventory_project.cmake")
        set_tests_properties(conversion-auto-project PROPERTIES TIMEOUT 240)
    endif()
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
