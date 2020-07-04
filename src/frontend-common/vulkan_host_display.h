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

class VulkanHostDisplay : public HostDisplay
{
public:
  VulkanHostDisplay();
  virtual ~VulkanHostDisplay();

  virtual RenderAPI GetRenderAPI() const override;
  virtual void* GetRenderDevice() const override;
  virtual void* GetRenderContext() const override;

  virtual bool HasRenderDevice() const override;
  virtual bool HasRenderSurface() const override;

  virtual bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device) override;
  virtual bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device) override;
  virtual void DestroyRenderDevice() override;

  virtual bool MakeRenderContextCurrent() override;
  virtual bool DoneRenderContextCurrent() override;

  virtual bool ChangeRenderWindow(const WindowInfo& new_wi) override;
  virtual void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  virtual void DestroyRenderSurface() override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* initial_data,
                                                    u32 initial_data_stride, bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  virtual void SetVSync(bool enabled) override;

  virtual bool Render() override;

  static std::vector<std::string> EnumerateAdapterNames();

protected:
  struct PushConstants
  {
    float src_rect_left;
    float src_rect_top;
    float src_rect_width;
    float src_rect_height;
  };

  // Can be overridden by frontends.
  virtual VkRenderPass GetRenderPassForDisplay() const;

  virtual bool CreateResources();
  virtual void DestroyResources();

  virtual bool CreateImGuiContext();
  virtual void DestroyImGuiContext();

  void RenderDisplay();
  void RenderImGui();
  void RenderSoftwareCursor();

  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     s32 texture_height, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                     s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture_handle);

  std::unique_ptr<Vulkan::SwapChain> m_swap_chain;

  VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline m_cursor_pipeline = VK_NULL_HANDLE;
  VkPipeline m_display_pipeline = VK_NULL_HANDLE;
  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;

  Vulkan::StagingTexture m_upload_staging_texture;
  Vulkan::StagingTexture m_readback_staging_texture;
};

} // namespace FrontendCommon