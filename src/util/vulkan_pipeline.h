// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_device.h"
#include "vulkan_loader.h"

class VulkanDevice;

class VulkanShader final : public GPUShader
{
  friend VulkanDevice;

public:
  ~VulkanShader() override;

  ALWAYS_INLINE VkShaderModule GetModule() const { return m_module; }

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

private:
  VulkanShader(GPUShaderStage stage, VkShaderModule mod);

  VkShaderModule m_module;
};

class VulkanPipeline final : public GPUPipeline
{
  friend VulkanDevice;

public:
  ~VulkanPipeline() override;

  ALWAYS_INLINE VkPipeline GetPipeline() const { return m_pipeline; }
  ALWAYS_INLINE Layout GetLayout() const { return m_layout; }
  ALWAYS_INLINE u8 GetVerticesPerPrimitive() const { return m_vertices_per_primitive; }

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

private:
  VulkanPipeline(VkPipeline pipeline, Layout layout, u8 vertices_per_primitive, RenderPassFlag render_pass_flags);

  VkPipeline m_pipeline;
  Layout m_layout;
  u8 m_vertices_per_primitive;
  RenderPassFlag m_render_pass_flags;
};
