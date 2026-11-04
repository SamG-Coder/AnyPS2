# Converted VU code targets an x86-64 microarchitecture level. The program
# verifies the host CPU before entering converted code.
set(ANYPS2_NATIVE_ISA "x86-64-v3" CACHE STRING "Host x86-64 level for converted VU code: x86-64, x86-64-v2, x86-64-v3 or x86-64-v4")
set_property(CACHE ANYPS2_NATIVE_ISA PROPERTY STRINGS x86-64 x86-64-v2 x86-64-v3 x86-64-v4)
# Reuse the native numeric implementation inside LLVM optimization. Imported
# helpers are internalized, so each module can inline them without duplicate
# public symbols or a second implementation of the arithmetic semantics.
function(anyps2_numeric_bitcode output)
    set(bitcode "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/anyps2-numeric.bc")
    if(NOT TARGET anyps2-numeric-bitcode)
        get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
        set(system_includes)
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            # Match CMake's selected MSVC toolset and Windows SDK. A standalone
            # clang invocation does not inherit MSBuild's include search paths.
            get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            get_filename_component(toolset "${compiler_dir}/../../.." ABSOLUTE)
            cmake_host_system_information(RESULT kits QUERY WINDOWS_REGISTRY
                "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
            list(APPEND system_includes -isystem "${toolset}/include")
            foreach(component ucrt shared um)
                list(APPEND system_includes -isystem
                    "${kits}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/${component}")
            endforeach()
        endif()
        file(GLOB numeric_headers CONFIGURE_DEPENDS "${root}/libraries/runtime/include/nt/*.hpp")
        if(CMAKE_MSVC_RUNTIME_LIBRARY)
            set(runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        else()
            set(runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        set(runtime_flag "$<$<STREQUAL:${runtime},MultiThreaded>:static>")
        string(APPEND runtime_flag "$<$<STREQUAL:${runtime},MultiThreadedDebug>:static_dbg>")
        string(APPEND runtime_flag "$<$<STREQUAL:${runtime},MultiThreadedDLL>:dll>")
        string(APPEND runtime_flag "$<$<STREQUAL:${runtime},MultiThreadedDebugDLL>:dll_dbg>")
        add_custom_command(OUTPUT "${bitcode}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
            COMMAND "${ANYPS2_CLANG}"
                -target x86_64-pc-windows-msvc -std=c++20 -O2 -fms-runtime-lib=${runtime_flag} -emit-llvm -c
                "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-DNDEBUG>"
                "${root}/libraries/ee_native/src/numeric.cpp"
                -I "${root}/libraries/runtime/include" ${system_includes} -o "${bitcode}"
            DEPENDS "${root}/libraries/ee_native/src/numeric.cpp" ${numeric_headers}
            VERBATIM)
        add_custom_target(anyps2-numeric-bitcode DEPENDS "${bitcode}")
        set_property(TARGET anyps2-numeric-bitcode PROPERTY ANYPS2_BITCODE_PATH "${bitcode}")
    endif()
    get_target_property(bitcode anyps2-numeric-bitcode ANYPS2_BITCODE_PATH)
    set(${output} "${bitcode}" PARENT_SCOPE)
endfunction()
