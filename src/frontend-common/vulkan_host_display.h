#pragma once
#include "common/timer.h"
#include "common/vulkan/loader.h"
#include "common/vulkan/stream_buffer.h"
#include "common/vulkan/swap_chain.h"
#include "common/window_info.h"
#include "core/host_display.h"
#include "postprocessing_chain.h"
#include <memory>
#include <string_view>

namespace Vulkan {
class StreamBuffer;
class SwapChain;
} // namespace Vulkan

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

  bool CreateRenderDevice(const WindowInfo& wi) override;
  bool InitializeRenderDevice() override;

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

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Format format, const void* data, u32 data_stride,
                                            bool dynamic = false) override;
  bool BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer, u32* out_pitch) override;
  void EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height) override;
  bool UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch) override;
  bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;
  bool SupportsTextureFormat(GPUTexture::Format format) const override;

  void SetVSync(bool enabled) override;

  bool Render(bool skip_present) override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        GPUTexture::Format* out_format) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

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
                                s32 final_height, Vulkan::Texture* texture, s32 texture_view_x, s32 texture_view_y,
                                s32 texture_view_width, s32 texture_view_height, u32 target_width, u32 target_height);

  VkRenderPass GetRenderPassForDisplay() const;

  bool CheckStagingBufferSize(u32 required_size);
  void DestroyStagingBuffer();

  bool CreateResources() override;
  void DestroyResources() override;

  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  void BeginSwapChainRenderPass(VkFramebuffer framebuffer, u32 width, u32 height);
  void RenderDisplay();
  void RenderImGui();
  void RenderSoftwareCursor();

  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, Vulkan::Texture* texture, s32 texture_view_x,
                     s32 texture_view_y, s32 texture_view_width, s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, GPUTexture* texture_handle);

  std::unique_ptr<Vulkan::SwapChain> m_swap_chain;

  VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline m_cursor_pipeline = VK_NULL_HANDLE;
  VkPipeline m_display_pipeline = VK_NULL_HANDLE;
  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;
  VkSampler m_border_sampler = VK_NULL_HANDLE;

  VmaAllocation m_readback_staging_allocation = VK_NULL_HANDLE;
  VkBuffer m_readback_staging_buffer = VK_NULL_HANDLE;
  u8* m_readback_staging_buffer_map = nullptr;
  u32 m_readback_staging_buffer_size = 0;
  bool m_is_adreno = false;

  VkDescriptorSetLayout m_post_process_descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_post_process_ubo_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_post_process_pipeline_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_post_process_ubo_pipeline_layout = VK_NULL_HANDLE;

  FrontendCommon::PostProcessingChain m_post_processing_chain;
  Vulkan::Texture m_post_processing_input_texture;
  VkFramebuffer m_post_processing_input_framebuffer = VK_NULL_HANDLE;
  Vulkan::StreamBuffer m_post_processing_ubo;
  std::vector<PostProcessingStage> m_post_processing_stages;
  Common::Timer m_post_processing_timer;
};
