# isobus-implement-sim: simulated ISOBUS implements on AgIsoStack++ (docs/PLAN.md).
cmake_minimum_required(VERSION 3.20)
project(isobus-implement-sim VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# AgIsoStack++ at a pinned upstream commit, fetched at configure time (PLAN.md section 2).
# Move the pin forward before each release; never patch the fetched tree.
set(AGISOSTACK_GIT_TAG "9e9e70c35ad10259db524c0ed8ec63cd8dca078e"
    CACHE STRING "AgIsoStack++ commit to build against (upstream HEAD of 2026-10-31)")

# CAN drivers the stack is built with. SocketCAN is the one that matters (PLAN.md section 2);
# the virtual driver lets the binaries configure and run on a machine without CAN, such as a
# macOS development box, with --driver virtual.
if(NOT CAN_DRIVER)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CAN_DRIVER "SocketCAN;VirtualCAN")
  else()
    # The stack bundles UV Software's MacCAN PCBUSB library (its own EULA, not MIT): a PEAK
    # PCAN-USB on the Mac, `--driver peak --interface 1`. Local builds only; releases are Linux.
    set(CAN_DRIVER "MacCANPCAN;VirtualCAN")
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

# ---------------------------------------------------------------------------------------------
# The simulator (phase 1 and on)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
                     URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(json)

add_library(implement_core STATIC
  src/profile.cpp
  src/model.cpp
  src/ddop.cpp
  src/events.cpp)
target_include_directories(implement_core PUBLIC src)
target_link_libraries(implement_core PUBLIC isobus::Isobus isobus::Utility nlohmann_json::nlohmann_json)
target_compile_options(implement_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
# The CAN drivers: the stack's, by its examples' factory, and the AOW link gateway client
add_library(can_drivers STATIC src/can_driver.cpp src/aow_link_driver.cpp
  ${agisostack_SOURCE_DIR}/examples/common/create_can_driver.cpp)
target_include_directories(can_drivers PUBLIC src ${agisostack_SOURCE_DIR}/examples/common)
target_link_libraries(can_drivers PUBLIC isobus::Isobus isobus::HardwareIntegration isobus::Utility Threads::Threads)

add_executable(implement
  src/main.cpp
  src/tc_client.cpp
  src/vt_client.cpp
  src/fault_driver.cpp
  src/commands.cpp)
target_link_libraries(implement PRIVATE can_drivers implement_core isobus::Isobus isobus::HardwareIntegration isobus::Utility Threads::Threads)
target_compile_options(implement PRIVATE -Wall -Wextra)

enable_testing()
add_executable(test_model tests/test_model.cpp)
target_link_libraries(test_model PRIVATE implement_core)
add_test(NAME model COMMAND test_model ${CMAKE_SOURCE_DIR}/profiles)

add_executable(ddop_check tools/ddop_check.cpp)
target_link_libraries(ddop_check PRIVATE isobus::Isobus isobus::Utility)

# candump for the stack's drivers: raw frames on a PEAK adapter on the Mac, or any driver
add_executable(can_sniff tools/can_sniff.cpp)
target_link_libraries(can_sniff PRIVATE can_drivers isobus::Isobus isobus::HardwareIntegration isobus::Utility)

# A headless virtual terminal server for the scenarios (phase 3)
add_executable(vt_stub tools/vt_stub.cpp)
target_link_libraries(vt_stub PRIVATE can_drivers isobus::Isobus isobus::HardwareIntegration isobus::Utility nlohmann_json::nlohmann_json Threads::Threads)

# A headless task controller for the scenarios (phase 5)
add_executable(tc_stub tools/tc_stub.cpp)
target_link_libraries(tc_stub PRIVATE can_drivers isobus::Isobus isobus::HardwareIntegration isobus::Utility nlohmann_json::nlohmann_json Threads::Threads)

# A simulated auxiliary input device, a joystick (phase 7)
add_executable(aux_input tools/aux_input.cpp)
target_link_libraries(aux_input PRIVATE can_drivers isobus::Isobus isobus::HardwareIntegration isobus::Utility nlohmann_json::nlohmann_json Threads::Threads)
