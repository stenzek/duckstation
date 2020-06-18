#pragma once
#include "common/vulkan/staging_texture.h"
#include "common/vulkan/swap_chain.h"
#include "common/window_info.h"
#include "core/host_display.h"
#include "vulkan_loader.h"
#include <memory>
#include <string_view>

namespace Vulkan {
class StreamBuffer;
class SwapChain;
} // namespace Vulkan

namespace FrontendCommon {

class VulkanHostDisplay
{
public:
  VulkanHostDisplay();
  ~VulkanHostDisplay();

  ALWAYS_INLINE HostDisplay::RenderAPI GetRenderAPI() const { return HostDisplay::RenderAPI::Vulkan; }
  ALWAYS_INLINE void* GetRenderDevice() const { return nullptr; }
  ALWAYS_INLINE void* GetRenderContext() const { return nullptr; }

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic);
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride);
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride);

  void SetVSync(bool enabled);

  bool BeginRender();
  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     u32 texture_height, u32 texture_view_x, u32 texture_view_y, u32 texture_view_width,
                     u32 texture_view_height, bool linear_filter);
  void RenderImGui();
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture_handle);
  void EndRenderAndPresent();

  bool CreateContextAndSwapChain(const WindowInfo& wi, bool debug_device);
  bool HasContext() const;
  void DestroyContext();

  void CreateShaderCache(std::string_view shader_cache_directory, bool debug_shaders);
  void DestroyShaderCache();

  bool CreateResources();
  void DestroyResources();

  bool CreateImGuiContext();
  void DestroyImGuiContext();

  ALWAYS_INLINE u32 GetSwapChainWidth() const { return m_swap_chain->GetWidth(); }
  ALWAYS_INLINE u32 GetSwapChainHeight() const { return m_swap_chain->GetHeight(); }
  ALWAYS_INLINE bool HasSwapChain() const { return static_cast<bool>(m_swap_chain); }

  bool RecreateSwapChain(const WindowInfo& new_wi);
  void ResizeSwapChain(u32 new_width, u32 new_height);
  void DestroySwapChain();

private:
  struct PushConstants
  {
    float src_rect_left;
    float src_rect_top;
    float src_rect_width;
    float src_rect_height;
  };

  std::unique_ptr<Vulkan::SwapChain> m_swap_chain;

  VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline m_software_cursor_pipeline = VK_NULL_HANDLE;
  VkPipeline m_display_pipeline = VK_NULL_HANDLE;
  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;

  Vulkan::StagingTexture m_upload_staging_texture;
  Vulkan::StagingTexture m_readback_staging_texture;
};

} // namespace FrontendCommon