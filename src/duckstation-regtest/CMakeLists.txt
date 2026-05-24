# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_executable(duckstation-regtest
  regtest_host.cpp
)

target_include_directories(duckstation-regtest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(duckstation-regtest PRIVATE core common scmversion)

add_core_resources(duckstation-regtest)