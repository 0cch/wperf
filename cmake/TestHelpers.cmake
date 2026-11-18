function(wperf_add_cpp_test target)
    add_executable(${target} ${ARGN})
    wperf_enable_cxx_options(${target})
    target_link_libraries(${target} PRIVATE wperf_core)
endfunction()

function(wperf_add_pwsh_test test_name script)
    add_test(NAME ${test_name}
             COMMAND pwsh -NoProfile -File "${WPERF_SOURCE_DIR}/${script}" ${ARGN})
endfunction()

function(wperf_set_test_timeout timeout)
    set_tests_properties(${ARGN} PROPERTIES TIMEOUT ${timeout})
endfunction()
