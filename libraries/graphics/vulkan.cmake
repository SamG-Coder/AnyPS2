# Shared native renderer build for the tools and converted executables.
function(anyps2_add_vulkan target)
    find_package(Vulkan REQUIRED COMPONENTS glslc)
    set(renderer_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    option(ANYPS2_LLVM_TEXTURES "Use build-time LLVM kernels for native texture import" OFF)
    set(llvm_shaders)
    if(ANYPS2_LLVM_TEXTURES)
        include("${renderer_root}/../../cmake/llvm_shaders.cmake")
        # First discover and validate the common LLVM tools.
        set(address_ir "${CMAKE_CURRENT_BINARY_DIR}/gpu-texture-address.ll")
        configure_file("${renderer_root}/shaders/texture_decode.ll" "${CMAKE_CURRENT_BINARY_DIR}/texture-decode.ll" COPYONLY)
        anyps2_compile_llvm_shader(texture_shader "${CMAKE_CURRENT_BINARY_DIR}/texture-decode.ll" "${address_ir}")
        execute_process(COMMAND "${ANYPS2_GPU_CLANG}" -print-resource-dir
            OUTPUT_VARIABLE resources OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        add_custom_command(OUTPUT "${address_ir}"
            COMMAND "${ANYPS2_GPU_CLANG}" -cc1 -triple spirv-unknown-vulkan1.3
                -x c++ -std=c++20 -ffreestanding -fconvergent-functions -DNDEBUG -emit-llvm -O2
                -I "${renderer_root}/include" -internal-isystem "${resources}/include"
                "${renderer_root}/src/gpu_texture_address.cpp" -o "${address_ir}"
            DEPENDS "${renderer_root}/src/gpu_texture_address.cpp" "${renderer_root}/include/nt/pixel_address.hpp" VERBATIM)
        set(texture_include "${CMAKE_CURRENT_BINARY_DIR}/texture-decode.inc")
        add_custom_command(OUTPUT "${texture_include}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${texture_shader}" "-DOUTPUT=${texture_include}"
                -P "${renderer_root}/../../cmake/embed_spirv.cmake"
            DEPENDS "${texture_shader}" "${renderer_root}/../../cmake/embed_spirv.cmake" VERBATIM)
        list(APPEND llvm_shaders "${texture_include}")
    endif()
    set(reference_shaders)
    if("REFERENCE" IN_LIST ARGN)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc"
            COMMAND Vulkan::glslc -mfmt=c "${renderer_root}/shaders/sprite.comp"
                -o "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc"
            DEPENDS "${renderer_root}/shaders/sprite.comp" "${renderer_root}/shaders/line_coverage.glsl" VERBATIM)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/scanout.inc"
            COMMAND Vulkan::glslc -mfmt=c "${renderer_root}/shaders/scanout.comp"
                -o "${CMAKE_CURRENT_BINARY_DIR}/scanout.inc"
            DEPENDS "${renderer_root}/shaders/scanout.comp" "${renderer_root}/shaders/display_composition.glsl" VERBATIM)
        set(reference_shaders "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc" "${CMAKE_CURRENT_BINARY_DIR}/scanout.inc")
    endif()
    set(native_shaders)
    foreach(shader native.vert native.frag present.vert present.frag field.comp palette.comp vector_assembly.comp)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc"
            COMMAND Vulkan::glslc -mfmt=c "${renderer_root}/shaders/${shader}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc"
            DEPENDS "${renderer_root}/shaders/${shader}" "${renderer_root}/shaders/line_coverage.glsl"
                "${renderer_root}/shaders/display_composition.glsl" VERBATIM)
        list(APPEND native_shaders "${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc")
    endforeach()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-interlock.frag.inc"
        COMMAND Vulkan::glslc --target-env=vulkan1.1 -DANYPS2_ORDERED_FRAGMENTS=1 -mfmt=c "${renderer_root}/shaders/native.frag"
            -o "${CMAKE_CURRENT_BINARY_DIR}/native-interlock.frag.inc"
        DEPENDS "${renderer_root}/shaders/native.frag" "${renderer_root}/shaders/line_coverage.glsl" VERBATIM)
    list(APPEND native_shaders "${CMAKE_CURRENT_BINARY_DIR}/native-interlock.frag.inc")
    add_library(${target} ${native_shaders} ${llvm_shaders} "${renderer_root}/src/native_raster.cpp" "${renderer_root}/src/native_vectors.cpp" "${renderer_root}/src/vulkan_renderer.cpp" ${reference_shaders})
    if(ANYPS2_LLVM_TEXTURES)
        target_compile_definitions(${target} PRIVATE ANYPS2_LLVM_TEXTURES=1)
    endif()
    if("REFERENCE" IN_LIST ARGN)
        target_compile_definitions(${target} PRIVATE ANYPS2_REFERENCE_RENDERER=1)
    endif()
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${renderer_root}/include" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${target} PUBLIC Vulkan::Vulkan)
endfunction()
