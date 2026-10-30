# Retain link evidence for generated Windows executables and inspect it after linking.
function(anyps2_check_native_output target)
    get_filename_component(link_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(ANYPS2_DUMPBIN NAMES dumpbin HINTS "${link_directory}" REQUIRED)
    set(audit_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_native_output.cmake")
    target_link_options(${target} PRIVATE "/MAP:$<TARGET_FILE:${target}>.map")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${audit_script}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:${target}>"
            "-DMAP_FILE=$<TARGET_FILE:${target}>.map"
            "-DDUMPBIN=${ANYPS2_DUMPBIN}"
            "-DREPORT=$<TARGET_FILE:${target}>.audit.txt"
            -P "${audit_script}" VERBATIM)
endfunction()
