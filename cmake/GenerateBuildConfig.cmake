# Writes the header that tells lossylab which source it was built from. Run as a
# script on every build (see CMakeLists.txt), so a new commit is picked up
# without reconfiguring. The header is only rewritten when its content changes.
#
# Inputs: SOURCE_DIR, OUTPUT_FILE, TEMPLATE_FILE, COMPILER, BUILD_TYPE.

execute_process(
        COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE LOSSYLAB_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_commit_result
)
if(NOT git_commit_result EQUAL 0)
    message(FATAL_ERROR "git rev-parse HEAD failed in ${SOURCE_DIR}: lossylab is built from a git checkout")
endif()

# A modified tracked file means the commit no longer identifies the code.
# Untracked files do not count.
execute_process(
        COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_status_result
)
if(NOT git_status_result EQUAL 0)
    message(FATAL_ERROR "git status failed in ${SOURCE_DIR}")
endif()
if(git_status STREQUAL "")
    set(LOSSYLAB_GIT_DIRTY 0)
else()
    set(LOSSYLAB_GIT_DIRTY 1)
endif()

set(LOSSYLAB_COMPILER "${COMPILER}")
set(LOSSYLAB_BUILD_TYPE "${BUILD_TYPE}")
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)
