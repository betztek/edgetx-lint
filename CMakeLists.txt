cmake_minimum_required(VERSION 3.20)

# edgetx-lint
#
# Builds ONE linter backend for ONE (radio, EdgeTX version) pair by reusing
# EdgeTX's own native ("simu") CMake build: the EdgeTX tree is added as a
# subdirectory in native mode and the backend links the very same object
# libraries (radiolib_native + simu_drivers) that EdgeTX's gtests-radio links.
# Nothing in the EdgeTX tree is patched.
#
# Usage:
#   cmake -S . -B build/tx16s-v2.12.5 -G Ninja -DLINT_RADIO=tx16s \
#         [-DEDGETX_SRC=/path/to/edgetx/checkout]
#   cmake --build build/tx16s-v2.12.5
#
# The frontend dispatcher (edgetx-lint) has no EdgeTX dependency and is built
# by every configuration.

set(EDGETX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/edgetx" CACHE PATH
    "EdgeTX source tree (git checkout of a release tag, with submodules)")
set(LINT_RADIO "tx16s" CACHE STRING "Radio to build the backend for (tx16s|pocket)")
option(LINT_FRONTEND_ONLY "Only build the dispatcher, no EdgeTX backend" OFF)

if(NOT EXISTS "${EDGETX_SRC}/radio/src/storage/yaml/yaml_parser.cpp")
  message(FATAL_ERROR "EDGETX_SRC=${EDGETX_SRC} is not an EdgeTX checkout "
                      "(run: git submodule update --init --recursive)")
endif()

# EdgeTX version of the tree we build against, parsed from its CMakeLists.txt
file(STRINGS "${EDGETX_SRC}/CMakeLists.txt" _etx_ver_lines
     REGEX "^set\\(VERSION_(MAJOR|MINOR|REVISION) \"[0-9]+\"\\)")
foreach(_l ${_etx_ver_lines})
  string(REGEX MATCH "VERSION_(MAJOR|MINOR|REVISION) \"([0-9]+)\"" _m "${_l}")
  set(EDGETX_VERSION_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
endforeach()
set(EDGETX_TAG "v${EDGETX_VERSION_MAJOR}.${EDGETX_VERSION_MINOR}.${EDGETX_VERSION_REVISION}")

# The native toolchain file only sets C++17 and a couple of warning flags.
if(NOT CMAKE_TOOLCHAIN_FILE AND NOT LINT_FRONTEND_ONLY)
  set(CMAKE_TOOLCHAIN_FILE "${EDGETX_SRC}/cmake/toolchain/native.cmake")
endif()

project(edgetx-lint C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LINT_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# ---------------------------------------------------------------- frontend
add_executable(edgetx-lint src/frontend/main.cpp)
target_compile_definitions(edgetx-lint PRIVATE LINT_VERSION="${LINT_VERSION}")
set_target_properties(edgetx-lint PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Release binaries should run on any recent glibc system without matching C++ runtime
target_link_options(edgetx-lint PRIVATE -static-libstdc++ -static-libgcc)

if(LINT_FRONTEND_ONLY)
  return()
endif()

# ---------------------------------------------------------------- backend
if(LINT_RADIO STREQUAL "tx16s")
  set(PCB X10)
  set(PCBREV TX16S)
elseif(LINT_RADIO STREQUAL "pocket")
  set(PCB X7)
  set(PCBREV POCKET)
else()
  message(FATAL_ERROR "Unsupported LINT_RADIO '${LINT_RADIO}' (tx16s|pocket)")
endif()

set(PCB ${PCB} CACHE STRING "EdgeTX PCB" FORCE)
set(PCBREV ${PCBREV} CACHE STRING "EdgeTX PCBREV" FORCE)
set(EdgeTX_SUPERBUILD OFF CACHE BOOL "" FORCE)
set(NATIVE_BUILD ON CACHE BOOL "" FORCE)
set(DISABLE_COMPANION ON CACHE BOOL "" FORCE)
set(TESTS_ASAN OFF CACHE BOOL "" FORCE)

add_subdirectory("${EDGETX_SRC}" edgetx EXCLUDE_FROM_ALL)

set(ETX_RADIO_SRC "${EDGETX_SRC}/radio/src")
get_directory_property(ETX_INCLUDES DIRECTORY "${ETX_RADIO_SRC}" INCLUDE_DIRECTORIES)
get_directory_property(ETX_DEFS DIRECTORY "${ETX_RADIO_SRC}" COMPILE_DEFINITIONS)
get_target_property(ETX_IFACE_OPTS radiolib_native INTERFACE_COMPILE_OPTIONS)

set(BACKEND_NAME "edgetx-lint-${LINT_RADIO}-${EDGETX_TAG}")

add_executable(edgetx-lint-backend
  src/backend/main.cpp
  src/backend/linter.cpp
  src/backend/describe.cpp
  src/backend/schema.cpp
  src/backend/report.cpp
  src/backend/simu_stubs.cpp
  $<TARGET_OBJECTS:radiolib_native>
  $<TARGET_OBJECTS:simu_drivers>
)
target_include_directories(edgetx-lint-backend PRIVATE
  ${ETX_INCLUDES}
  "${ETX_RADIO_SRC}/targets/simu"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backend")
target_compile_definitions(edgetx-lint-backend PRIVATE
  ${ETX_DEFS}
  LINT_VERSION="${LINT_VERSION}"
  LINT_RADIO="${LINT_RADIO}"
  LINT_EDGETX_TAG="${EDGETX_TAG}")
target_compile_options(edgetx-lint-backend PRIVATE ${ETX_IFACE_OPTS} -Wno-unused-parameter)
add_dependencies(edgetx-lint-backend radiolib_native simu_drivers)
find_package(Threads REQUIRED)
target_link_libraries(edgetx-lint-backend PRIVATE Threads::Threads)
target_link_options(edgetx-lint-backend PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(edgetx-lint-backend PROPERTIES
  OUTPUT_NAME ${BACKEND_NAME}
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/libexec/edgetx-lint)

message(STATUS "edgetx-lint backend: ${BACKEND_NAME} (PCB=${PCB} PCBREV=${PCBREV})")
