// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"

#include <sstream>
#include <string>

class ShaderGen
{
public:
  ShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch);
  ~ShaderGen();

  static bool UseGLSLBindingLayout();

  std::string GenerateScreenQuadVertexShader(float z = 0.0f);
  std::string GenerateUVQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateCopyFragmentShader();

  std::string GenerateImGuiVertexShader();
  std::string GenerateImGuiFragmentShader();

protected:
  ALWAYS_INLINE bool IsVulkan() const { return (m_render_api == RenderAPI::Vulkan); }
  ALWAYS_INLINE bool IsMetal() const { return (m_render_api == RenderAPI::Metal); }

  const char* GetInterpolationQualifier(bool interface_block, bool centroid_interpolation, bool sample_interpolation,
                                        bool is_out) const;

#ifdef ENABLE_OPENGL
  void SetGLSLVersionString();
#endif

  void DefineMacro(std::stringstream& ss, const char* name, bool enabled);
  void DefineMacro(std::stringstream& ss, const char* name, s32 value);
  void WriteHeader(std::stringstream& ss);
  void WriteUniformBufferDeclaration(std::stringstream& ss, bool push_constant_on_vulkan);
  void DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members,
                            bool push_constant_on_vulkan);
  void DeclareTexture(std::stringstream& ss, const char* name, u32 index, bool multisampled = false,
                      bool is_int = false, bool is_unsigned = false);
  void DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int, bool is_unsigned);
  void DeclareVertexEntryPoint(std::stringstream& ss, const std::initializer_list<const char*>& attributes,
                               u32 num_color_outputs, u32 num_texcoord_outputs,
                               const std::initializer_list<std::pair<const char*, const char*>>& additional_outputs,
                               bool declare_vertex_id = false, const char* output_block_suffix = "", bool msaa = false,
                               bool ssaa = false, bool noperspective_color = false);
  void DeclareFragmentEntryPoint(std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
                                 const std::initializer_list<std::pair<const char*, const char*>>& additional_inputs,
                                 bool declare_fragcoord = false, u32 num_color_outputs = 1, bool depth_output = false,
                                 bool msaa = false, bool ssaa = false, bool declare_sample_id = false,
                                 bool noperspective_color = false, bool feedback_loop = false);

  RenderAPI m_render_api;
  bool m_glsl;
  bool m_spirv;
  bool m_supports_dual_source_blend;
  bool m_supports_framebuffer_fetch;
  bool m_use_glsl_interface_blocks;
  bool m_use_glsl_binding_layout;
  bool m_has_uniform_buffer = false;

  std::string m_glsl_version_string;
};
