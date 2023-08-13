// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_device.h"
#include "vulkan_loader.h"

class VulkanDevice;

class VulkanShader final : public GPUShader
{
  friend VulkanDevice;

public:
  ~VulkanShader() override;

  ALWAYS_INLINE VkShaderModule GetModule() const { return m_module; }

  void SetDebugName(const std::string_view& name) override;

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

  void SetDebugName(const std::string_view& name) override;

private:
  VulkanPipeline(VkPipeline pipeline, Layout layout);

  VkPipeline m_pipeline;
  Layout m_layout;
};
