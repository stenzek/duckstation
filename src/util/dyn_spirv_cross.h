// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "spirv_cross_c.h"

class Error;

#define SPIRV_CROSS_FUNCTIONS(X)                                                                                       \
  X(spvc_context_create)                                                                                               \
  X(spvc_context_destroy)                                                                                              \
  X(spvc_context_set_error_callback)                                                                                   \
  X(spvc_context_parse_spirv)                                                                                          \
  X(spvc_context_create_compiler)                                                                                      \
  X(spvc_compiler_create_compiler_options)                                                                             \
  X(spvc_compiler_create_shader_resources)                                                                             \
  X(spvc_compiler_get_execution_model)                                                                                 \
  X(spvc_compiler_options_set_bool)                                                                                    \
  X(spvc_compiler_options_set_uint)                                                                                    \
  X(spvc_compiler_install_compiler_options)                                                                            \
  X(spvc_compiler_require_extension)                                                                                   \
  X(spvc_compiler_compile)                                                                                             \
  X(spvc_compiler_set_name)                                                                                            \
  X(spvc_compiler_set_decoration)                                                                                      \
  X(spvc_compiler_get_decoration)                                                                                      \
  X(spvc_compiler_get_member_name)                                                                                     \
  X(spvc_compiler_get_member_decoration)                                                                               \
  X(spvc_compiler_get_declared_struct_size)                                                                            \
  X(spvc_compiler_get_declared_struct_member_size)                                                                     \
  X(spvc_compiler_get_type_handle)                                                                                     \
  X(spvc_resources_get_resource_list_for_type)

#ifdef _WIN32
#define SPIRV_CROSS_HLSL_FUNCTIONS(X)                                                                                  \
  X(spvc_compiler_hlsl_add_resource_binding)                                                                           \
  X(spvc_compiler_hlsl_add_vertex_attribute_remap)
#else
#define SPIRV_CROSS_HLSL_FUNCTIONS(X)
#endif
#ifdef __APPLE__
#define SPIRV_CROSS_MSL_FUNCTIONS(X) X(spvc_compiler_msl_add_resource_binding)
#else
#define SPIRV_CROSS_MSL_FUNCTIONS(X)
#endif

namespace dyn_libs {
extern bool OpenSpirvCross(Error* error);

#define ADD_FUNC(F) extern decltype(&::F) F;
SPIRV_CROSS_FUNCTIONS(ADD_FUNC)
SPIRV_CROSS_HLSL_FUNCTIONS(ADD_FUNC)
SPIRV_CROSS_MSL_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

} // namespace dyn_libs