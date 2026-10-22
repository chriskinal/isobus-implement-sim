# Applies a patch to a fetched source tree once. FetchContent's patch step reruns when its command
# changes, and the step must then be a no-op for a tree that already carries the patch.
#   cmake -DSOURCE_DIR=<dir> -DPATCH=<file> -P apply_patch.cmake
execute_process(COMMAND git apply --check --reverse "${PATCH}" WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(applied EQUAL 0)
  message(STATUS "patch already applied: ${PATCH}")
  return()
endif()
execute_process(COMMAND git apply "${PATCH}" WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "patch failed: ${PATCH}")
endif()
