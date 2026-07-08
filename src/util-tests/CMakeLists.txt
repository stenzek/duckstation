# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_executable(util-tests
  animated_image_tests.cpp
  cd_image_tests.cpp
  elf_parser_tests.cpp
  cue_parser_tests.cpp
  image_tests.cpp
  ini_settings_interface_tests.cpp
  object_archive_tests.cpp
  stub_host.cpp
)

target_include_directories(util-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(util-tests PRIVATE util gtest gtest_main)
