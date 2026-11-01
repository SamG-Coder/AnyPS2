include(FetchContent)
if(NOT CMAKE_C_COMPILER_LOADED)
    enable_language(C)
endif()
# Private static dependency; preserve the parent project's shared-library policy.
set(BUILD_SHARED_LIBS OFF)
set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "Build HIDAPI tools" FORCE)
set(HIDAPI_BUILD_PP_DATA_DUMP OFF CACHE BOOL "Build HIDAPI diagnostics" FORCE)
set(HIDAPI_WITH_TESTS OFF CACHE BOOL "Build HIDAPI tests" FORCE)
FetchContent_Declare(anyps2_hidapi
    URL https://codeload.github.com/libusb/hidapi/tar.gz/dbff4ea89f55a572aeb0c53a7b32ea70853ec260
    URL_HASH SHA256=15fb5a0b07bafe80fd3e702d8e55e9c8eb6f3525434c53635ba89426a3213069
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(anyps2_hidapi)
# In this pinned release hid.c includes the descriptor implementation. Building
# that implementation separately also produces duplicate public definitions.
get_target_property(hidapi_sources hidapi_winapi SOURCES)
list(FILTER hidapi_sources EXCLUDE REGEX "(^|/)hidapi_descriptor_reconstruct\\.c$")
set_property(TARGET hidapi_winapi PROPERTY SOURCES "${hidapi_sources}")
