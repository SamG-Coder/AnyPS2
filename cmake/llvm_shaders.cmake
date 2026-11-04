# Build-time LLVM -> Vulkan SPIR-V. Nothing in this file installs a runtime
# compiler or changes the CPU conversion backend.
include_guard(GLOBAL)
function(anyps2_compile_llvm_shader result source)
    cmake_parse_arguments(shader "STRUCTURIZE" "" "" ${ARGN})
    find_program(ANYPS2_GPU_CLANG NAMES clang REQUIRED)
    find_program(ANYPS2_SPIRV_LLC NAMES llc REQUIRED)
    find_program(ANYPS2_LLVM_LINK NAMES llvm-link REQUIRED)
    find_program(ANYPS2_LLVM_OPT NAMES opt REQUIRED)
    find_program(ANYPS2_SPIRV_VAL NAMES spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    execute_process(COMMAND "${ANYPS2_SPIRV_LLC}" --version
        OUTPUT_VARIABLE targets RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT targets MATCHES "spirv")
        message(FATAL_ERROR "ANYPS2_SPIRV_LLC must include the LLVM SPIR-V backend")
    endif()
    execute_process(COMMAND "${ANYPS2_GPU_CLANG}" -print-resource-dir
        OUTPUT_VARIABLE resources OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(numeric "${CMAKE_BINARY_DIR}/gpu-numeric.ll")
    if(NOT TARGET anyps2-gpu-numeric)
        add_custom_command(OUTPUT "${numeric}"
            COMMAND "${ANYPS2_GPU_CLANG}" -cc1 -triple spirv-unknown-vulkan1.3
                -x c++ -std=c++20 -ffreestanding -fconvergent-functions -DNDEBUG
                -emit-llvm -O2 -I "${root}/libraries/runtime/include"
                -internal-isystem "${resources}/include"
                "${root}/libraries/ee_native/src/gpu_numeric.cpp" -o "${numeric}"
            DEPENDS "${root}/libraries/ee_native/src/gpu_numeric.cpp"
                "${root}/libraries/runtime/include/nt/finite_arithmetic.hpp"
                "${root}/libraries/runtime/include/nt/ee_float.hpp"
                "${root}/libraries/runtime/include/nt/ee_vector_math.hpp"
                "${root}/libraries/runtime/include/nt/vu_quotient.hpp"
                "${root}/libraries/runtime/include/nt/vu_extended.hpp"
                "${root}/libraries/runtime/include/nt/compiler_attributes.hpp" VERBATIM)
        add_custom_target(anyps2-gpu-numeric DEPENDS "${numeric}")
    endif()
    set(linked "${source}.linked.bc")
    set(optimized "${source}.optimized.bc")
    set(output "${source}.spv")
    set(passes "internalize,default<O2>")
    set(codegen_options "")
    # Arithmetic graphs with nested helper exits need explicit structured CFG
    # lowering. Keep it selectable: LLVM 22's second structurization of an
    # already simple resource kernel can create invalid selection merges.
    # structurizecfg only rewrites conditional branches; a switch left by the
    # optimizer becomes a branch on undef, which drivers may legally reduce to
    # a shader that writes nothing, so switches are lowered first. Machine CSE
    # can move an instruction between OpSelectionMerge and its branch in these
    # graphs. Every variant must pass spirv-val before it can enter a runtime binary.
    if(shader_STRUCTURIZE)
        string(APPEND passes ",lower-switch,structurizecfg")
        list(APPEND codegen_options -disable-machine-cse)
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${ANYPS2_LLVM_LINK}" "${source}" "${numeric}" ${shader_UNPARSED_ARGUMENTS} -o "${linked}"
        COMMAND "${ANYPS2_LLVM_OPT}" "-passes=${passes}" -internalize-public-api-list=main
            "${linked}" -o "${optimized}"
        COMMAND "${ANYPS2_SPIRV_LLC}" -mtriple=spirv-unknown-vulkan1.3 -O2 ${codegen_options} -filetype=obj
            "${optimized}" -o "${output}"
        COMMAND "${ANYPS2_SPIRV_VAL}" --target-env vulkan1.3 "${output}"
        DEPENDS "${source}" "${numeric}" ${shader_UNPARSED_ARGUMENTS} anyps2-gpu-numeric
        BYPRODUCTS "${linked}" "${optimized}" VERBATIM)
    set(${result} "${output}" PARENT_SCOPE)
endfunction()
