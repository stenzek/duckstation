# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction

add_executable(core-tests
  analog_controller_tests.cpp
  analog_joystick_tests.cpp
  bus_tests.cpp
  cdrom_tests.cpp
  cheats_tests.cpp
  controller_helpers_tests.cpp
  controller_test_utils.cpp
  controller_test_utils.h
  cpu_disasm_tests.cpp
  cpu_execution_tests.cpp
  cpu_pgxp_tests.cpp
  cpu_test_harness.cpp
  ddgo_controller_tests.cpp
  cpu_test_harness.h
  digital_controller_tests.cpp
  dma_tests.cpp
  fullscreenui_widgets_tests.cpp
  game_list_tests.cpp
  gpu_sw_rasterizer_tests.cpp
  gpu_tests.cpp
  gte_tests.cpp
  gte_timing_tests.cpp
  jogcon_tests.cpp
  lightgun_controller_tests.cpp
  macro_sequence_tests.cpp
  mdec_tests.cpp
  negcon_base_tests.cpp
  pad_tests.cpp
  playstation_mouse_tests.cpp
  psxdiag_tests.cpp
  spu_tests.cpp
  stub_host.cpp
  timers_tests.cpp
)

target_include_directories(core-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core-tests PRIVATE core gtest gtest_main)
add_core_resources(core-tests)
