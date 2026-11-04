function(anyps2_add_input target)
    if(NOT TARGET SDL3::SDL3-static)
        include(FetchContent)
        if(NOT CMAKE_C_COMPILER_LOADED)
            enable_language(C)
        endif()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        foreach(subsystem AUDIO VIDEO RENDER CAMERA GPU)
            set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
        endforeach()
        FetchContent_Declare(anyps2_sdl
            URL https://codeload.github.com/libsdl-org/SDL/tar.gz/bd9f6ca9ff0e9509f49980fc1b70837f54b6f244
            URL_HASH SHA256=6d820fec9af8b0a9d1fe386dd9c41a401df0a564ea941570140391da765b36c3
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(anyps2_sdl)
    endif()
    add_library(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/gamepad.cpp"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/pad_script.cpp")
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../docs/third-party/SDL-license.txt"
                   "${CMAKE_BINARY_DIR}/SDL-license.txt" COPYONLY)
endfunction()
