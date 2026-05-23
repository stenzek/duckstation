# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_executable(common-tests
  binary_reader_writer_tests.cpp
  bitutils_tests.cpp
  file_system_tests.cpp
  gsvector_tests.cpp
  gsvector_yuvtorgb_test.cpp
  hash_tests.cpp
  heap_array_tests.cpp
  misc_tests.cpp
  path_tests.cpp
  rectangle_tests.cpp
  small_string_tests.cpp
  string_pool_tests.cpp
  string_tests.cpp
)

target_include_directories(common-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common-tests PRIVATE common gtest gtest_main)
