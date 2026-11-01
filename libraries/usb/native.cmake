function(anyps2_add_usb target)
    add_library(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/host.cpp"
                          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/hid_reports.cpp"
                          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/windows_hid.cpp")
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(WIN32)
        if(NOT TARGET hidapi::winapi)
            include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/hidapi.cmake")
        endif()
        target_link_libraries(${target} PRIVATE setupapi hid cfgmgr32 hidapi::winapi)
        # Every output directory that receives the static USB library also
        # receives the dependency notice for redistribution with executables.
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../docs/third-party/hidapi-license.txt"
                       "${CMAKE_BINARY_DIR}/HIDAPI-BSD.txt" COPYONLY)
    endif()
endfunction()
