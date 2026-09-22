# Each test_*.cpp file is self-contained (plain assert() checks and its own
# main()) and becomes its own executable and its own ctest entry, so a failure
# names the file without any filtering. New files are picked up automatically.
file(GLOB LOSSYLAB_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

foreach(test_src IN LISTS LOSSYLAB_TEST_SOURCES)
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE lossylab::lossylab)
    lossylab_set_warnings(${test_name})

    # Tests resolve fixtures through this, so they run from any working directory.
    target_compile_definitions(${test_name}
            PRIVATE LOSSYLAB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

    # assert() must stay live even under a Release build's -DNDEBUG.
    target_compile_options(${test_name} PRIVATE -UNDEBUG)

    add_test(NAME ${test_name} COMMAND ${test_name})

    # Tests guarded on an optional FFmpeg capability report a skip rather than a
    # pass when the build does not provide it.
    set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
