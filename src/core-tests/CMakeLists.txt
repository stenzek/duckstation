# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction

add_executable(core-tests
  ../core/cheats.cpp
  ../core/cpu_disasm.cpp
  ../core/cpu_types.cpp
  ../util/zip_helpers.cpp
  cheats_tests.cpp
  cpu_disasm_tests.cpp
  stub_cpu.cpp
)

target_include_directories(core-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core-tests PRIVATE common imgui libzip::zip gtest gtest_main)

if(APPLE)
  target_compile_options(core-tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(core-tests PRIVATE -Wl,-dead_strip)
elseif(NOT MSVC)
  target_compile_options(core-tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(core-tests PRIVATE -Wl,--gc-sections)
endif()
