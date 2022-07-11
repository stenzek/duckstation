#pragma once
#include "common/dimensional_array.h"
#include "common/vulkan/staging_texture.h"
#include "common/vulkan/stream_buffer.h"
#include "common/vulkan/texture.h"
#include "gpu_hw.h"
#include "texture_replacements.h"
#include <array>
#include <memory>
#include <tuple>

class GPU_HW_Vulkan : public GPU_HW
{
public:
  GPU_HW_Vulkan();
  ~GPU_HW_Vulkan() override;

  GPURenderer GetRendererType() const override;

  bool Initialize() override;
  void Reset(bool clear_vram) override;
  bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display) override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

protected:
  void ClearDisplay() override;
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void UpdateVRAMReadTexture() override;
  void UpdateDepthBufferFromMaskBit() override;
  void ClearDepthBuffer() override;
  void SetScissorFromDrawingArea() override;
  void MapBatchVertexPointer(u32 required_vertices) override;
  void UnmapBatchVertexPointer(u32 used_vertices) override;
  void UploadUniformBuffer(const void* data, u32 data_size) override;
  void DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices) override;

private:
  enum : u32
  {
    MAX_PUSH_CONSTANTS_SIZE = 64,
    TEXTURE_REPLACEMENT_BUFFER_SIZE = 64 * 1024 * 1024
  };
  void SetCapabilities();
  void DestroyResources();

  ALWAYS_INLINE bool InRenderPass() const { return (m_current_render_pass != VK_NULL_HANDLE); }
  void BeginRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer, u32 x, u32 y, u32 width, u32 height,
                       const VkClearValue* clear_value = nullptr);
  void BeginVRAMRenderPass();
  void EndRenderPass();
  void ExecuteCommandBuffer(bool wait_for_completion, bool restore_state);

  bool CreatePipelineLayouts();
  bool CreateSamplers();

  bool CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  bool CreateVertexBuffer();
  bool CreateUniformBuffer();
  bool CreateTextureBuffer();

  bool CompilePipelines();
  void DestroyPipelines();

  bool CreateTextureReplacementStreamBuffer();

  bool BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, u32 dst_x, u32 dst_y, u32 width, u32 height);

  void DownsampleFramebuffer(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height);
  void DownsampleFramebufferBoxFilter(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height);
  void DownsampleFramebufferAdaptive(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height);

  VkRenderPass m_current_render_pass = VK_NULL_HANDLE;

  VkRenderPass m_vram_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_vram_update_depth_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_display_load_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_display_discard_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_vram_readback_render_pass = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_batch_descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_single_sampler_descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_vram_write_descriptor_set_layout = VK_NULL_HANDLE;

  VkPipelineLayout m_batch_pipeline_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_no_samplers_pipeline_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_single_sampler_pipeline_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_vram_write_pipeline_layout = VK_NULL_HANDLE;

  Vulkan::Texture m_vram_texture;
  Vulkan::Texture m_vram_depth_texture;
  Vulkan::Texture m_vram_read_texture;
  Vulkan::Texture m_vram_readback_texture;
  Vulkan::StagingTexture m_vram_readback_staging_texture;
  Vulkan::Texture m_display_texture;
  bool m_use_ssbos_for_vram_writes = false;

  VkFramebuffer m_vram_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_vram_update_depth_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_vram_readback_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_display_framebuffer = VK_NULL_HANDLE;

  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;
  VkSampler m_trilinear_sampler = VK_NULL_HANDLE;

  VkDescriptorSet m_batch_descriptor_set = VK_NULL_HANDLE;
  VkDescriptorSet m_vram_copy_descriptor_set = VK_NULL_HANDLE;
  VkDescriptorSet m_vram_read_descriptor_set = VK_NULL_HANDLE;
  VkDescriptorSet m_vram_write_descriptor_set = VK_NULL_HANDLE;
  VkDescriptorSet m_display_descriptor_set = VK_NULL_HANDLE;

  Vulkan::StreamBuffer m_vertex_stream_buffer;
  Vulkan::StreamBuffer m_uniform_stream_buffer;
  Vulkan::StreamBuffer m_texture_stream_buffer;

  u32 m_current_uniform_buffer_offset = 0;
  VkBufferView m_texture_stream_buffer_view = VK_NULL_HANDLE;

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  DimensionalArray<VkPipeline, 2, 2, 5, 9, 4, 3> m_batch_pipelines{};

  // [wrapped][interlaced]
  DimensionalArray<VkPipeline, 2, 2> m_vram_fill_pipelines{};

  // [depth_test]
  std::array<VkPipeline, 2> m_vram_write_pipelines{};
  std::array<VkPipeline, 2> m_vram_copy_pipelines{};

  VkPipeline m_vram_readback_pipeline = VK_NULL_HANDLE;
  VkPipeline m_vram_update_depth_pipeline = VK_NULL_HANDLE;

  // [depth_24][interlace_mode]
  DimensionalArray<VkPipeline, 3, 2> m_display_pipelines{};

  // texture replacements
  Vulkan::Texture m_vram_write_replacement_texture;
  Vulkan::StreamBuffer m_texture_replacment_stream_buffer;

  // downsampling
  Vulkan::Texture m_downsample_texture;
  VkRenderPass m_downsample_render_pass = VK_NULL_HANDLE;
  Vulkan::Texture m_downsample_weight_texture;
  VkRenderPass m_downsample_weight_render_pass = VK_NULL_HANDLE;
  VkFramebuffer m_downsample_weight_framebuffer = VK_NULL_HANDLE;

  struct SmoothMipView
  {
    VkImageView image_view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };
  std::vector<SmoothMipView> m_downsample_mip_views;

  VkPipelineLayout m_downsample_pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_downsample_composite_descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout m_downsample_composite_pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSet m_downsample_composite_descriptor_set = VK_NULL_HANDLE;
  VkPipeline m_downsample_first_pass_pipeline = VK_NULL_HANDLE;
  VkPipeline m_downsample_mid_pass_pipeline = VK_NULL_HANDLE;
  VkPipeline m_downsample_blur_pass_pipeline = VK_NULL_HANDLE;
  VkPipeline m_downsample_composite_pass_pipeline = VK_NULL_HANDLE;
};
