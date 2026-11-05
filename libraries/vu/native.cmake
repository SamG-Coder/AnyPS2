include("${CMAKE_CURRENT_LIST_DIR}/../ee_native/numeric.cmake")
function(anyps2_bind_vu target)
    set(ANYPS2_VU_MODULE "" CACHE FILEPATH "Ahead-of-time VU LLVM module to link")
    set(ANYPS2_VU1_MODULE "" CACHE FILEPATH "Ahead-of-time VU1 LLVM module to link")
    set(ANYPS2_VU1_REGIONS "" CACHE PATH "Directory of converted VU1 vector-region LLVM modules (ps2-vu-loop-probe --regions)")
    foreach(unit IN ITEMS 0 1)
        if(unit EQUAL 0)
            set(module "${ANYPS2_VU_MODULE}")
            set(definition ANYPS2_NATIVE_VU)
        else()
            set(module "${ANYPS2_VU1_MODULE}")
            set(definition ANYPS2_NATIVE_VU1)
        endif()
        if(NOT module)
            continue()
        endif()
        if(NOT EXISTS "${module}")
            message(FATAL_ERROR "Compiled VU${unit} module input does not exist")
        endif()
        set(object "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vu${unit}-native.obj")
        anyps2_numeric_bitcode(numeric_bitcode)
        # Guard-heavy converted blocks look cold to LLVM without profile data.
        # Allow small arithmetic bodies to inline there; retain larger slow paths.
        add_custom_command(OUTPUT "${object}"
        COMMAND "${ANYPS2_CLANG}" -x ir -target x86_64-pc-windows-msvc
            -Werror -Wno-override-module -O3 -march=${ANYPS2_NATIVE_ISA} -mllvm -inline-cold-callsite-threshold=150 -c "${module}"
            -Xclang -mlink-builtin-bitcode -Xclang "${numeric_bitcode}" -o "${object}"
        DEPENDS "${module}" "${numeric_bitcode}" VERBATIM)
        add_dependencies(${target} anyps2-numeric-bitcode)
        target_sources(${target} PRIVATE "${object}")
        target_include_directories(${target} PRIVATE "${ANYPS2_ROOT}/libraries/vu/include")
        target_compile_definitions(${target} PRIVATE ${definition}=1 ANYPS2_NATIVE_ISA="${ANYPS2_NATIVE_ISA}")
        if(unit EQUAL 1 AND ANYPS2_VU1_REGIONS)
            file(GLOB region_modules "${ANYPS2_VU1_REGIONS}/*.ll")
            if(NOT region_modules)
                message(FATAL_ERROR "ANYPS2_VU1_REGIONS contains no LLVM modules")
            endif()
            foreach(region_module IN LISTS region_modules)
                get_filename_component(region_name "${region_module}" NAME_WE)
                get_filename_component(region_part "${region_module}" NAME)
                string(REPLACE ".ll" "" region_part "${region_part}")
                set(region_object "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vu1-region-${region_part}.obj")
                # Region boundaries call the original block code's numeric helpers.
                add_custom_command(OUTPUT "${region_object}"
                    COMMAND "${ANYPS2_CLANG}" -x ir -target x86_64-pc-windows-msvc
                        -Werror -Wno-override-module -O3 -march=${ANYPS2_NATIVE_ISA} -c "${region_module}"
                        -Xclang -mlink-builtin-bitcode -Xclang "${numeric_bitcode}" -o "${region_object}"
                    DEPENDS "${region_module}" "${numeric_bitcode}" VERBATIM)
                target_sources(${target} PRIVATE "${region_object}")
            endforeach()
            # Generated projects include only native library files; provide the
            # region runtime here when the source tree has not defined it.
            if(NOT TARGET nt_render_runtime)
                add_library(nt_render_runtime "${ANYPS2_ROOT}/libraries/render_runtime/src/element_abi.cpp"
                    "${ANYPS2_ROOT}/libraries/render_runtime/src/vector_region.cpp")
                target_include_directories(nt_render_runtime PUBLIC "${ANYPS2_ROOT}/libraries/render_runtime/include"
                    "${ANYPS2_ROOT}/libraries/vu/include")
                target_compile_features(nt_render_runtime PUBLIC cxx_std_20)
            endif()
            target_link_libraries(${target} PRIVATE nt_render_runtime)
            # CPU element kernels use the exact element helpers.
            target_sources(${target} PRIVATE "${ANYPS2_ROOT}/libraries/ee_native/src/gpu_numeric.cpp")
            target_compile_definitions(${target} PRIVATE ANYPS2_NATIVE_VU1_REGIONS=1)
        endif()
    endforeach()
endfunction()
