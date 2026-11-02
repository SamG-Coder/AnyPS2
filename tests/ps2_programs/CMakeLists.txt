cmake_minimum_required(VERSION 3.24)
project(AnyPS2OriginalPs2Programs LANGUAGES NONE)
if(NOT PS2DEV)
    message(FATAL_ERROR "Pass -DPS2DEV=<PS2DEV toolchain directory>")
endif()
find_program(ee_compiler mips64r5900el-ps2-elf-g++ HINTS "${PS2DEV}/ee/bin" NO_DEFAULT_PATH REQUIRED)
execute_process(COMMAND "${ee_compiler}" --version RESULT_VARIABLE status OUTPUT_VARIABLE version ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "PS2 cross-compiler cannot start (${status}). On Windows put matching 32-bit MinGW host DLLs on PATH. ${error}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/compiler-version.txt" "${version}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cases.json" catalog)
string(JSON count LENGTH "${catalog}" cases)
math(EXPR last "${count}-1")
foreach(index RANGE ${last})
    string(JSON name GET "${catalog}" cases ${index} name)
    string(JSON case_id GET "${catalog}" cases ${index} id)
    foreach(opt 0 2)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}-O${opt}.elf")
        add_custom_command(OUTPUT "${output}"
            BYPRODUCTS "${output}.map"
            COMMAND "${ee_compiler}" -std=c++20 -O${opt} -G0 -mno-abicalls -fno-pic
                -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-unwind-tables
                -fno-asynchronous-unwind-tables -nostdlib -Wall -Wextra -Werror
                "-DTEST_CASE_ID=${case_id}" "${CMAKE_CURRENT_SOURCE_DIR}/start.S"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp"
                "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/link.ld,-Map,${output}.map,--build-id=none"
                -o "${output}"
            DEPENDS start.S runtime.cpp protocol.hpp "${name}.cpp" link.ld cases.json "${ee_compiler}"
            VERBATIM)
        add_custom_target(${name}-O${opt} ALL DEPENDS "${output}")
    endforeach()
endforeach()
