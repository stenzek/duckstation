#pragma once
#include "common/vulkan/staging_texture.h"
#include "common/vulkan/stream_buffer.h"
#include "common/vulkan/swap_chain.h"
#include "common/window_info.h"
#include "core/host_display.h"
#include "postprocessing_chain.h"
#include "vulkan_loader.h"
#include <memory>
#include <string_view>

namespace Vulkan {
class StreamBuffer;
class SwapChain;
} // namespace Vulkan

namespace FrontendCommon {

class VulkanHostDisplay final : public HostDisplay
{
public:
  VulkanHostDisplay();
  ~VulkanHostDisplay();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool HasRenderDevice() const override;
  bool HasRenderSurface() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                                  bool threaded_presentation) override;
  bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                                      bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  bool MakeRenderContextCurrent() override;
  bool DoneRenderContextCurrent() override;

  bool ChangeRenderWindow(const WindowInfo& new_wi) override;
  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool SupportsFullscreen() const override;
  bool IsFullscreen() override;
  bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) override;
  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroyRenderSurface() override;

  bool SetPostProcessingChain(const std::string_view& config) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    HostDisplayPixelFormat format, const void* data, u32 data_stride,
                                                    bool dynamic = false) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;
  bool DownloadTexture(const void* texture_handle, HostDisplayPixelFormat texture_format, u32 x, u32 y, u32 width,
                       u32 height, void* out_data, u32 out_data_stride) override;

  bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override;
  bool BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                             u32* out_pitch) override;
  void EndSetDisplayPixels() override;

  void SetVSync(bool enabled) override;

  bool Render() override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                HostDisplayPixelFormat* out_format) override;

  static AdapterAndModeList StaticGetAdapterAndModeList(const WindowInfo* wi);

protected:
  struct PushConstants
  {
    float src_rect_left;
    float src_rect_top;
    float src_rect_width;
    float src_rect_height;
  };

  struct PostProcessingStage
  {
    PostProcessingStage() = default;
    PostProcessingStage(PostProcessingStage&& move);
    ~PostProcessingStage();

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFramebuffer output_framebuffer = VK_NULL_HANDLE;
    Vulkan::Texture output_texture;
    u32 uniforms_size = 0;
  };

  bool CheckPostProcessingRenderTargets(u32 target_width, u32 target_height);
  void ApplyPostProcessingChain(VkFramebuffer target_fb, s32 final_left, s32 final_top, s32 final_width,
                                s32 final_height, void* texture_handle, u32 texture_width, s32 texture_height,
                                s32 texture_view_x, s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                u32 target_width, u32 target_height);

  // Can be overridden by frontends.
  VkRenderPass GetRenderPassForDisplay() const;

  bool CreateResources() override;
  void DestroyResources() override;

  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  void BeginSwapChainRenderPass(VkFramebuffer framebuffer, u32 width, u32 height);
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

  Vulkan::Texture m_display_pixels_texture;
  Vulkan::StagingTexture m_upload_staging_texture;
  Vulkan::StagingTexture m_readback_staging_texture;

  VkDescriptorSetLayout m_post_process_descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_post_process_ubo_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_post_process_pipeline_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_post_process_ubo_pipeline_layout = VK_NULL_HANDLE;

  PostProcessingChain m_post_processing_chain;
  Vulkan::Texture m_post_processing_input_texture;
  VkFramebuffer m_post_processing_input_framebuffer = VK_NULL_HANDLE;
  Vulkan::StreamBuffer m_post_processing_ubo;
  std::vector<PostProcessingStage> m_post_processing_stages;
};

} // namespace FrontendCommon