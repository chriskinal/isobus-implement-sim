# isobus-implement-sim: simulated ISOBUS implements on AgIsoStack++ (docs/PLAN.md).
cmake_minimum_required(VERSION 3.20)
project(isobus-implement-sim VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# AgIsoStack++ at a pinned upstream commit, fetched at configure time (PLAN.md section 2).
# Move the pin forward before each release; never patch the fetched tree.
set(AGISOSTACK_GIT_TAG "42ea80e14e4ad1340697bda632b7f6abdcd54826"
    CACHE STRING "AgIsoStack++ commit to build against (upstream HEAD of 2026-10-19)")

# CAN drivers the stack is built with. SocketCAN is the one that matters (PLAN.md section 2);
# the virtual driver lets the binaries configure and run on a machine without CAN, such as a
# macOS development box, with --driver virtual.
if(NOT CAN_DRIVER)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CAN_DRIVER "SocketCAN;VirtualCAN")
  else()
    set(CAN_DRIVER "VirtualCAN")
  endif()
endif()

include(FetchContent)
set(BUILD_EXAMPLES OFF)
set(BUILD_TESTING OFF)
FetchContent_Declare(agisostack
  GIT_REPOSITORY https://github.com/Open-Agriculture/AgIsoStack-plus-plus.git
  GIT_TAG ${AGISOSTACK_GIT_TAG})
FetchContent_MakeAvailable(agisostack)

# Phase 0 (PLAN.md section 8): the stack's seeder example, built unchanged from the fetched
# source. Its CMakeLists skips find_package when BUILD_EXAMPLES is set, and the stack's targets
# are in this build already.
set(BUILD_EXAMPLES ON)
add_subdirectory(${agisostack_SOURCE_DIR}/examples/seeder_example
                 ${CMAKE_BINARY_DIR}/seeder_example)
