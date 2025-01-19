// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_backend.h"
#include "gpu_hw_texture_cache.h"

#include "util/gpu_device.h"

#include "common/dimensional_array.h"
#include "common/gsvector.h"

#include <limits>
#include <tuple>
#include <utility>

namespace PostProcessing {
class Chain;
}

// TODO: Move to cpp
// TODO: Rename to GPUHWBackend, preserved to avoid conflicts.
class GPU_HW final : public GPUBackend
{
public:
  enum class BatchRenderMode : u8
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent,
    ShaderBlend,

    MaxCount,
  };

  enum class BatchTextureMode : u8
  {
    Palette4Bit,
    Palette8Bit,
    Direct16Bit,
    PageTexture,
    Disabled,

    SpritePalette4Bit,
    SpritePalette8Bit,
    SpriteDirect16Bit,
    SpritePageTexture,

    MaxCount,

    SpriteStart = SpritePalette4Bit,
  };
  static_assert(static_cast<u8>(BatchTextureMode::Palette4Bit) == static_cast<u8>(GPUTextureMode::Palette4Bit) &&
                static_cast<u8>(BatchTextureMode::Palette8Bit) == static_cast<u8>(GPUTextureMode::Palette8Bit) &&
                static_cast<u8>(BatchTextureMode::Direct16Bit) == static_cast<u8>(GPUTextureMode::Direct16Bit));

  static constexpr GSVector4i VRAM_SIZE_RECT = GSVector4i::cxpr(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  static constexpr GSVector4i INVALID_RECT =
    GSVector4i::cxpr(std::numeric_limits<s32>::max(), std::numeric_limits<s32>::max(), std::numeric_limits<s32>::min(),
                     std::numeric_limits<s32>::min());

  GPU_HW(GPUPresenter& presenter);
  ~GPU_HW() override;

  bool Initialize(bool upload_vram, Error* error) override;

  u32 GetResolutionScale() const override;

  void RestoreDeviceContext() override;
  void FlushRender() override;

  bool UpdateSettings(const GPUSettings& old_settings, Error* error) override;
  void UpdatePostProcessingSettings(bool force_reload) override;

  bool UpdateResolutionScale(Error* error) override;

  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced_rendering, u8 active_line_lsb) override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                bool check_mask) override;
  void ClearCache() override;
  void OnBufferSwapped() override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd) override;
  void DrawSprite(const GPUBackendDrawRectangleCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawPreciseLine(const GPUBackendDrawPreciseLineCommand* cmd) override;

  void DrawingAreaChanged() override;
  void ClearVRAM() override;

  void LoadState(const GPUBackendLoadStateCommand* cmd) override;

  bool AllocateMemorySaveState(System::MemorySaveState& mss, Error* error) override;
  void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss) override;

  void UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd) override;

private:
  enum : u32
  {
    MAX_BATCH_VERTEX_COUNTER_IDS = 65536 - 2,
    MAX_VERTICES_FOR_RECTANGLE = 6 * (((MAX_PRIMITIVE_WIDTH + (TEXTURE_PAGE_WIDTH - 1)) / TEXTURE_PAGE_WIDTH) + 1u) *
                                 (((MAX_PRIMITIVE_HEIGHT + (TEXTURE_PAGE_HEIGHT - 1)) / TEXTURE_PAGE_HEIGHT) + 1u),
    NUM_TEXTURE_MODES = static_cast<u32>(BatchTextureMode::MaxCount),
    INVALID_DRAW_MODE_BITS = 0xFFFFFFFFu,
  };
  enum : u8
  {
    TEXPAGE_DIRTY_DRAWN_RECT = (1 << 0),
    TEXPAGE_DIRTY_WRITTEN_RECT = (1 << 1),
    TEXPAGE_DIRTY_PAGE_RECT = (1 << 2),
    TEXPAGE_DIRTY_ONLY_UV_RECT = (1 << 3),
  };

  static_assert(GPUDevice::MIN_TEXEL_BUFFER_ELEMENTS >= (VRAM_WIDTH * VRAM_HEIGHT));

  struct alignas(16) BatchVertex
  {
    float x;
    float y;
    float z;
    float w;
    u32 color;
    u32 texpage;
    u16 u; // 16-bit texcoords are needed for 256 extent rectangles
    u16 v;
    u32 uv_limits;

    void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 packed_texcoord, u32 uv_limits_);
    void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 u_, u16 v_, u32 uv_limits_);
    static u32 PackUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v);
    void SetUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v);
  };

  struct BatchConfig
  {
    BatchTextureMode texture_mode;
    GPUTransparencyMode transparency_mode;
    bool dithering;
    bool interlacing;
    bool set_mask_while_drawing; // NOTE: could be replaced with ubo u_set_mask_while drawing if needed
    bool check_mask_before_draw;
    bool use_depth_buffer;
    bool sprite_mode;

    // Returns the render mode for this batch.
    BatchRenderMode GetRenderMode() const;
  };

  struct BatchUBOData
  {
    u32 u_texture_window[4]; // and_x, and_y, or_x, or_y
    float u_src_alpha_factor;
    float u_dst_alpha_factor;
    u32 u_interlaced_displayed_field;
    u32 u_set_mask_while_drawing;
    float u_resolution_scale;
    float u_rcp_resolution_scale;
    float u_resolution_scale_minus_one;
  };

  struct RendererStats
  {
    u32 num_batches;
    u32 num_vram_read_texture_updates;
    u32 num_uniform_buffer_updates;
  };

  /// Returns true if a depth buffer should be created.
  GPUTexture::Format GetDepthBufferFormat() const;

  bool CreateBuffers(Error* error);
  void ClearFramebuffer();
  void DestroyBuffers();

  bool CompileCommonShaders(Error* error);
  bool CompilePipelines(Error* error);
  bool CompileResolutionDependentPipelines(Error* error);
  bool CompileDownsamplePipelines(Error* error);

  void PrintSettingsToLog();
  void CheckSettings();

  void UpdateVRAMReadTexture(bool drawn, bool written);
  void UpdateDepthBufferFromMaskBit();
  void CopyAndClearDepthBuffer();
  void ClearDepthBuffer();
  void SetScissor();
  void SetVRAMRenderTarget();
  void DeactivateROV();
  void MapGPUBuffer(u32 required_vertices, u32 required_indices);
  void UnmapGPUBuffer(u32 used_vertices, u32 used_indices);
  void DrawBatchVertices(BatchRenderMode render_mode, u32 num_indices, u32 base_index, u32 base_vertex,
                         const GPUTextureCache::Source* texture);

  u32 CalculateResolutionScale() const;
  GPUDownsampleMode GetDownsampleMode(u32 resolution_scale) const;

  bool ShouldDrawWithSoftwareRenderer() const;

  bool IsUsingMultisampling() const;
  bool IsUsingDownsampling(const GPUBackendUpdateDisplayCommand* cmd) const;

  void SetFullVRAMDirtyRectangle();
  void ClearVRAMDirtyRectangle();

  void AddWrittenRectangle(const GSVector4i rect);
  void AddDrawnRectangle(const GSVector4i rect);
  void AddUnclampedDrawnRectangle(const GSVector4i rect);
  void SetTexPageChangedOnOverlap(const GSVector4i update_rect);

  void CheckForTexPageOverlap(const GPUBackendDrawCommand* cmd, GSVector4i uv_rect);
  bool ShouldCheckForTexPageOverlap() const;

  bool IsFlushed() const;

  void EnsureVertexBufferSpace(u32 required_vertices, u32 required_indices);
  void EnsureVertexBufferSpaceForCommand(const GPUBackendDrawCommand* cmd);
  void PrepareDraw(const GPUBackendDrawCommand* cmd);
  bool BeginPolygonDraw(const GPUBackendDrawCommand* cmd, std::array<BatchVertex, 4>& vertices, u32& num_vertices,
                        GSVector4i& clamped_draw_rect_012, GSVector4i& clamped_draw_rect_123);
  void FinishPolygonDraw(const GPUBackendDrawCommand* cmd, std::array<BatchVertex, 4>& vertices, u32 num_vertices,
                         bool is_precise, bool is_3d, const GSVector4i clamped_draw_rect_012,
                         const GSVector4i clamped_draw_rect_123);
  void ResetBatchVertexDepth();

  /// Returns the value to be written to the depth buffer for the current operation for mask bit emulation.
  float GetCurrentNormalizedVertexDepth() const;

  /// Returns if the draw needs to be broken into opaque/transparent passes.
  bool NeedsTwoPassRendering() const;

  /// Returns true if the draw is going to use shader blending/framebuffer fetch.
  bool NeedsShaderBlending(GPUTransparencyMode transparency, BatchTextureMode texture, bool check_mask) const;

  void DownloadVRAMFromGPU(u32 x, u32 y, u32 width, u32 height);
  void UpdateVRAMOnGPU(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_pitch, bool set_mask,
                       bool check_mask, const GSVector4i bounds);
  bool BlitVRAMReplacementTexture(GPUTexture* tex, u32 dst_x, u32 dst_y, u32 width, u32 height);

  /// Expands a line into two triangles.
  void DrawLine(const GSVector4 bounds, u32 col0, u32 col1, float depth);

  /// Computes partial derivatives and area for the given triangle. Needed for sprite/line detection.
  static void ComputeUVPartialDerivatives(const BatchVertex* vertices, float* dudx, float* dudy, float* dvdx,
                                          float* dvdy, float* xy_area, s32* uv_area);

  /// Handles quads with flipped texture coordinate directions.
  void HandleFlippedQuadTextureCoordinates(const GPUBackendDrawCommand* cmd, BatchVertex* vertices);
  bool IsPossibleSpritePolygon(const BatchVertex* vertices) const;
  bool ExpandLineTriangles(BatchVertex* vertices);

  /// Computes polygon U/V boundaries, and for overlap with the current texture page.
  void ComputePolygonUVLimits(const GPUBackendDrawCommand* cmd, BatchVertex* vertices, u32 num_vertices);

  /// Sets the depth test flag for PGXP depth buffering.
  void SetBatchDepthBuffer(const GPUBackendDrawCommand* cmd, bool enabled);
  void CheckForDepthClear(const GPUBackendDrawCommand* cmd, const BatchVertex* vertices, u32 num_vertices);
  void SetBatchSpriteMode(const GPUBackendDrawCommand* cmd, bool enabled);

  void UpdateDownsamplingLevels();

  void DownsampleFramebuffer();
  void DownsampleFramebufferAdaptive(GPUTexture* source, u32 left, u32 top, u32 width, u32 height);
  void DownsampleFramebufferBoxFilter(GPUTexture* source, u32 left, u32 top, u32 width, u32 height);

  void LoadInternalPostProcessing();

  std::unique_ptr<GPUTexture> m_vram_texture;
  std::unique_ptr<GPUTexture> m_vram_depth_texture;
  std::unique_ptr<GPUTexture> m_vram_depth_copy_texture;
  std::unique_ptr<GPUTexture> m_vram_read_texture;
  std::unique_ptr<GPUTexture> m_vram_readback_texture;
  std::unique_ptr<GPUDownloadTexture> m_vram_readback_download_texture;

  std::unique_ptr<GPUTextureBuffer> m_vram_upload_buffer;
  std::unique_ptr<GPUTexture> m_vram_write_texture;

  BatchVertex* m_batch_vertex_ptr = nullptr;
  u16* m_batch_index_ptr = nullptr;
  u32 m_batch_base_vertex = 0;
  u32 m_batch_base_index = 0;
  u16 m_batch_vertex_count = 0;
  u16 m_batch_index_count = 0;
  u16 m_batch_vertex_space = 0;
  u16 m_batch_index_space = 0;
  s32 m_current_depth = 0;
  float m_last_depth_z = 1.0f;

  u8 m_resolution_scale = 1;
  u8 m_multisamples = 1;

  GPUTextureFilter m_texture_filtering = GPUTextureFilter::Nearest;
  GPUTextureFilter m_sprite_texture_filtering = GPUTextureFilter::Nearest;
  GPULineDetectMode m_line_detect_mode = GPULineDetectMode::Disabled;
  GPUDownsampleMode m_downsample_mode = GPUDownsampleMode::Disabled;
  GPUWireframeMode m_wireframe_mode = GPUWireframeMode::Disabled;

  bool m_supports_dual_source_blend : 1 = false;
  bool m_supports_framebuffer_fetch : 1 = false;
  bool m_true_color : 1 = true;
  bool m_pgxp_depth_buffer : 1 = false;
  bool m_clamp_uvs : 1 = false;
  bool m_compute_uv_range : 1 = false;
  bool m_allow_sprite_mode : 1 = false;
  bool m_allow_shader_blend : 1 = false;
  bool m_prefer_shader_blend : 1 = false;
  bool m_use_rov_for_shader_blend : 1 = false;
  bool m_write_mask_as_depth : 1 = false;
  bool m_depth_was_copied : 1 = false;
  bool m_texture_window_active : 1 = false;
  bool m_rov_active : 1 = false;

  bool m_use_texture_cache : 1 = false;
  bool m_texture_dumping : 1 = false;

  u8 m_texpage_dirty = 0;

  bool m_batch_ubo_dirty = true;
  bool m_drawing_area_changed = true;
  BatchConfig m_batch = {};
  GPUTextureCache::SourceKey m_texture_cache_key = {};

  // Changed state
  BatchUBOData m_batch_ubo_data = {};

  // Bounding box of VRAM area that the GPU has drawn into.
  GSVector4i m_vram_dirty_draw_rect = INVALID_RECT;
  GSVector4i m_vram_dirty_write_rect = INVALID_RECT; // TODO: Don't use in TC mode, should be kept at zero.
  GSVector4i m_current_uv_rect = INVALID_RECT;
  GSVector4i m_current_draw_rect = INVALID_RECT;
  alignas(8) s32 m_current_texture_page_offset[2] = {};

  union
  {
    struct
    {
      // NOTE: Only the texture-related bits should be used here, the others are not validated.
      GPUDrawModeReg mode_reg;
      GPUTexturePaletteReg palette_reg;
    };

    u32 bits = INVALID_DRAW_MODE_BITS;
  } m_draw_mode = {};

  GPUTextureWindow m_texture_window_bits = {};

  std::unique_ptr<GPUPipeline> m_wireframe_pipeline;

  // [wrapped][interlaced]
  DimensionalArray<std::unique_ptr<GPUPipeline>, 2, 2> m_vram_fill_pipelines{};

  // [depth_test]
  std::array<std::unique_ptr<GPUPipeline>, 2> m_vram_write_pipelines{};
  std::array<std::unique_ptr<GPUPipeline>, 2> m_vram_copy_pipelines{};

  std::unique_ptr<GPUPipeline> m_vram_readback_pipeline;
  std::unique_ptr<GPUPipeline> m_vram_update_depth_pipeline;
  std::unique_ptr<GPUPipeline> m_vram_write_replacement_pipeline;

  std::array<std::unique_ptr<GPUPipeline>, 3> m_vram_extract_pipeline; // [24bit, 2=depth]
  std::unique_ptr<GPUTexture> m_vram_extract_texture;
  std::unique_ptr<GPUTexture> m_vram_extract_depth_texture;
  std::unique_ptr<GPUPipeline> m_copy_depth_pipeline;
  std::unique_ptr<PostProcessing::Chain> m_internal_postfx;

  std::unique_ptr<GPUTexture> m_downsample_texture;
  std::unique_ptr<GPUPipeline> m_downsample_pass_pipeline;
  std::unique_ptr<GPUPipeline> m_downsample_blur_pipeline;
  std::unique_ptr<GPUPipeline> m_downsample_composite_pipeline;
  std::unique_ptr<GPUSampler> m_downsample_lod_sampler;
  std::unique_ptr<GPUSampler> m_downsample_composite_sampler;
  u32 m_downsample_scale_or_levels = 0;

  // [depth_test][transparency_mode][render_mode][texture_mode][dithering][interlacing][check_mask]
  DimensionalArray<std::unique_ptr<GPUPipeline>, 2, 2, 2, NUM_TEXTURE_MODES, 5, 5, 2> m_batch_pipelines{};

  // common shaders
  std::unique_ptr<GPUShader> m_fullscreen_quad_vertex_shader;
  std::unique_ptr<GPUShader> m_screen_quad_vertex_shader;
};
