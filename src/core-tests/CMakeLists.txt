# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction

add_executable(core-tests
  ../core/cpu_disasm.cpp
  ../core/cpu_types.cpp
  cpu_disasm_tests.cpp
  stub_cpu.cpp
)

target_include_directories(core-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core-tests PRIVATE common gtest gtest_main)
