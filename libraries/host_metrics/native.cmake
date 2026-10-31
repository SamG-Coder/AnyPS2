function(anyps2_add_host_metrics target)
  add_library(${target}
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/profile_sources.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/counter_intervals.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/etw_counters.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/live_counters.cpp")
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include")
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_link_libraries(${target} PRIVATE advapi32 ole32)
endfunction()
