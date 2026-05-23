# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.
#
# Unless otherwise specified, other files supporting the build system are covered under
# the same terms.
#

cmake_minimum_required(VERSION 3.19)
project(duckstation C CXX)

# Policy settings.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "DuckStation does not support in-tree builds. Please make a build directory that is not the source"
                      "directory and generate your CMake project there using either `cmake -B build_directory` or by "
                      "running cmake from the build directory.")
endif()

if(NOT CMAKE_BUILD_TYPE MATCHES "Debug|Devel|MinSizeRel|RelWithDebInfo|Release")
  message(FATAL_ERROR "CMAKE_BUILD_TYPE not set. Please set it first.")
endif()

# Pull in modules.
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeModules/")
include(DuckStationUtils)

# Detect system attributes.
detect_operating_system()
detect_compiler()
detect_architecture()
detect_page_size()
detect_cache_line_size()

# Build options. Depends on system attributes.
include(DuckStationBuildOptions)
include(DuckStationDependencies)
include(DuckStationCompilerRequirement)

# Enable PIC on Linux, otherwise the builds do not support ASLR.
if(LINUX OR BSD)
  include(CheckPIESupported)
  check_pie_supported()
  set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)
endif()

# Force debug symbols for Linux builds.
add_debug_symbol_flag(CMAKE_C_FLAGS_RELEASE)
add_debug_symbol_flag(CMAKE_CXX_FLAGS_RELEASE)

# Warning disables.
if(COMPILER_CLANG OR COMPILER_CLANG_CL OR COMPILER_GCC)
  include(CheckCXXFlag)
  check_cxx_flag(-Wall COMPILER_SUPPORTS_WALL)
  check_cxx_flag(-Wno-invalid-offsetof COMPILER_SUPPORTS_OFFSETOF)
endif()

# We don't need exceptions, disable them to save a bit of code size.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")

# Rewrite paths in macros to be relative to the source directory.
# Helpful for reproducible builds.
if("${CMAKE_BUILD_TYPE}" STREQUAL "Release" AND NOT CMAKE_GENERATOR MATCHES "Xcode" AND
   (COMPILER_CLANG OR COMPILER_CLANG_CL OR COMPILER_GCC))
  file(RELATIVE_PATH source_dir_remap "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
  string(REGEX REPLACE "\/+$" "" source_dir_remap "${source_dir_remap}")
  set(source_dir_remap_str "\"${CMAKE_SOURCE_DIR}\"=\"${source_dir_remap}\"")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffile-prefix-map=${source_dir_remap_str}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffile-prefix-map=${source_dir_remap_str}")
endif()

# Write binaries to a seperate directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

# Enable large file support on Linux 32-bit platforms.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_definitions("-D_FILE_OFFSET_BITS=64")
endif()

# Optional unit tests.
if(BUILD_TESTS)
  enable_testing()
endif()

# Recursively include the source tree.
add_subdirectory(dep)
add_subdirectory(src)

# Output build summary.
include(DuckStationBuildSummary)
