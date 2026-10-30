# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction

add_executable(core-tests
  cdrom_tests.cpp
  cheats_tests.cpp
  controller_helpers_tests.cpp
  cpu_disasm_tests.cpp
  cpu_execution_tests.cpp
  cpu_pgxp_tests.cpp
  cpu_test_harness.cpp
  cpu_test_harness.h
  dma_tests.cpp
  gpu_sw_rasterizer_tests.cpp
  gpu_tests.cpp
  gte_tests.cpp
  gte_timing_tests.cpp
  lightgun_controller_tests.cpp
  mdec_tests.cpp
  negcon_base_tests.cpp
  psxdiag_tests.cpp
  spu_tests.cpp
  stub_host.cpp
  timers_tests.cpp
)

target_include_directories(core-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core-tests PRIVATE core gtest gtest_main)
