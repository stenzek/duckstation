# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_custom_target(generate_scmversion
                  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.sh"
                  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/scmversion.cpp")

add_library(scmversion
  "${CMAKE_CURRENT_BINARY_DIR}/scmversion.cpp"
  scmversion.h
)
add_dependencies(scmversion generate_scmversion)
