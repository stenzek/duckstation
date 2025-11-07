// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "shaderc/shaderc.h"

#define DYN_SHADERC_FUNCTIONS(X)                                                                                       \
  X(shaderc_compiler_initialize)                                                                                       \
  X(shaderc_compiler_release)                                                                                          \
  X(shaderc_compile_options_initialize)                                                                                \
  X(shaderc_compile_options_release)                                                                                   \
  X(shaderc_compile_options_set_source_language)                                                                       \
  X(shaderc_compile_options_set_generate_debug_info)                                                                   \
  X(shaderc_compile_options_set_optimization_level)                                                                    \
  X(shaderc_compile_options_set_target_env)                                                                            \
  X(shaderc_compilation_status_to_string)                                                                              \
  X(shaderc_compile_into_spv)                                                                                          \
  X(shaderc_result_release)                                                                                            \
  X(shaderc_result_get_length)                                                                                         \
  X(shaderc_result_get_num_warnings)                                                                                   \
  X(shaderc_result_get_bytes)                                                                                          \
  X(shaderc_result_get_compilation_status)                                                                             \
  X(shaderc_result_get_error_message)                                                                                  \
  X(shaderc_optimize_spv)

namespace dyn_libs {

extern bool OpenShaderc(Error* error);

#define ADD_FUNC(F) extern decltype(&::F) F;
DYN_SHADERC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

extern shaderc_compiler_t g_shaderc_compiler;

} // namespace dyn_libs
