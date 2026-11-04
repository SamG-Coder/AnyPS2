include("${CMAKE_CURRENT_LIST_DIR}/../ee_native/numeric.cmake")
function(anyps2_bind_vu target)
    set(ANYPS2_VU_MODULE "" CACHE FILEPATH "Ahead-of-time VU LLVM module to link")
    set(ANYPS2_VU1_MODULE "" CACHE FILEPATH "Ahead-of-time VU1 LLVM module to link")
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
    endforeach()
endfunction()
