# Shared native renderer build for the tools and converted executables.
function(anyps2_add_vulkan target)
    find_package(Vulkan REQUIRED COMPONENTS glslc)
    set(renderer_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc"
        COMMAND Vulkan::glslc -mfmt=c "${renderer_root}/shaders/sprite.comp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc"
        DEPENDS "${renderer_root}/shaders/sprite.comp" VERBATIM)
    add_library(${target} "${renderer_root}/src/vulkan_renderer.cpp" "${CMAKE_CURRENT_BINARY_DIR}/sprite.inc")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${renderer_root}/include" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${target} PUBLIC Vulkan::Vulkan)
endfunction()
