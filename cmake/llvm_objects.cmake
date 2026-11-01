function(anyps2_add_llvm_library target project)
    set(ANYPS2_LLVM_OPT_LEVEL "2" CACHE STRING "LLVM optimization level (0, 1, 2, 3)")
    if(NOT ANYPS2_LLVM_OPT_LEVEL MATCHES "^[0-3]$")
        message(FATAL_ERROR "ANYPS2_LLVM_OPT_LEVEL must be 0, 1, 2 or 3")
    endif()
    option(ANYPS2_LLVM_REPORTS "Write per-module LLVM optimization and compile-time reports" OFF)
    # Resolve even an explicitly supplied command name to a real file so that a
    # toolchain upgrade invalidates cached objects through the dependency below.
    if(ANYPS2_CLANG)
        find_program(llvm_compiler NAMES "${ANYPS2_CLANG}" REQUIRED NO_CACHE)
    else()
        find_program(llvm_compiler NAMES clang REQUIRED NO_CACHE)
    endif()
    set(ANYPS2_CLANG "${llvm_compiler}" PARENT_SCOPE)
    # MSBuild does not retain executable-file inputs consistently in custom-build
    # tracking. A stable text stamp preserves toolchain invalidation without
    # making every object rebuild on every invocation.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${llvm_compiler}")
    file(TIMESTAMP "${llvm_compiler}" compiler_time "%Y-%m-%dT%H:%M:%S.%fZ" UTC)
    file(SIZE "${llvm_compiler}" compiler_size)
    set(toolchain "${CMAKE_CURRENT_BINARY_DIR}/${target}-toolchain.txt")
    file(CONFIGURE OUTPUT "${toolchain}"
        CONTENT "@llvm_compiler@\n@compiler_time@\n@compiler_size@\nO@ANYPS2_LLVM_OPT_LEVEL@\nreports=@ANYPS2_LLVM_REPORTS@\n" @ONLY)
    set(manifest "${project}/llvm-modules.txt")
    if(EXISTS "${manifest}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
        file(STRINGS "${manifest}" modules)
    else()
        # Existing generated projects remain buildable.
        set(modules generated.ll)
    endif()
    if(NOT modules)
        message(FATAL_ERROR "Empty LLVM module manifest: ${manifest}")
    endif()
    set(objects)
    set(tasks)
    foreach(module IN LISTS modules)
        if(NOT module MATCHES "^[A-Za-z0-9_-]+\\.ll$" OR NOT EXISTS "${project}/${module}")
            message(FATAL_ERROR "Invalid or missing LLVM module: ${module}")
        endif()
        get_filename_component(stem "${module}" NAME_WE)
        set(object "${CMAKE_CURRENT_BINARY_DIR}/${target}-${stem}.obj")
        set(reports)
        set(report_files)
        if(ANYPS2_LLVM_REPORTS)
            list(APPEND reports -fsave-optimization-record
                "-foptimization-record-file=${object}.opt.yaml" "-ftime-trace=${object}.time.json")
            list(APPEND report_files "${object}.opt.yaml" "${object}.time.json")
        endif()
        add_custom_command(OUTPUT "${object}"
            BYPRODUCTS ${report_files}
            COMMAND "${llvm_compiler}" -target x86_64-pc-windows-msvc
                -Werror -Wno-override-module "-O${ANYPS2_LLVM_OPT_LEVEL}" ${reports}
                -c "${project}/${module}" -o "${object}"
            DEPENDS "${project}/${module}" "${toolchain}" VERBATIM)
        # Separate targets expose the independent work to Visual Studio/MSBuild.
        add_custom_target(${target}_${stem} DEPENDS "${object}")
        list(APPEND tasks ${target}_${stem})
        list(APPEND objects "${object}")
    endforeach()
    add_library(${target} STATIC ${objects})
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    add_dependencies(${target} ${tasks})
endfunction()
