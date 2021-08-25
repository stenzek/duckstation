#pragma once
#include "common/heap_array.h"
#include "gpu.h"
#include "host_display.h"
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class GPU_SW_Backend;
struct GPUBackendCommand;
struct GPUBackendDrawCommand;

class GPU_HW : public GPU
{
public:
  enum class BatchRenderMode : u8
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  enum class InterlacedRenderMode : u8
  {
    None,
    InterleavedFields,
    SeparateFields
  };

  GPU_HW();
  virtual ~GPU_HW();

  virtual bool Initialize(HostDisplay* host_display) override;
  virtual void Reset(bool clear_vram) override;
  virtual bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display) override;

  void UpdateResolutionScale() override final;
  std::tuple<u32, u32> GetEffectiveDisplayResolution(bool scaled = true) override final;
  std::tuple<u32, u32> GetFullDisplayResolution(bool scaled = true) override final;

protected:
  enum : u32
  {
    VRAM_UPDATE_TEXTURE_BUFFER_SIZE = 4 * 1024 * 1024,
    VERTEX_BUFFER_SIZE = 4 * 1024 * 1024,
    UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024,
    MAX_BATCH_VERTEX_COUNTER_IDS = 65536 - 2,
    MAX_VERTICES_FOR_RECTANGLE = 6 * (((MAX_PRIMITIVE_WIDTH + (TEXTURE_PAGE_WIDTH - 1)) / TEXTURE_PAGE_WIDTH) + 1u) *
                                 (((MAX_PRIMITIVE_HEIGHT + (TEXTURE_PAGE_HEIGHT - 1)) / TEXTURE_PAGE_HEIGHT) + 1u)
  };
  static_assert(VRAM_UPDATE_TEXTURE_BUFFER_SIZE >= VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));

  struct BatchVertex
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

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 packed_texcoord,
                           u32 uv_limits_)
    {
      Set(x_, y_, z_, w_, color_, texpage_, packed_texcoord & 0xFF, (packed_texcoord >> 8), uv_limits_);
    }

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 u_, u16 v_,
                           u32 uv_limits_)
    {
      x = x_;
      y = y_;
      z = z_;
      w = w_;
      color = color_;
      texpage = texpage_;
      u = u_;
      v = v_;
      uv_limits = uv_limits_;
    }

    ALWAYS_INLINE static u32 PackUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
    {
      return min_u | (min_v << 8) | (max_u << 16) | (max_v << 24);
    }

    ALWAYS_INLINE void SetUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
    {
      uv_limits = PackUVLimits(min_u, max_u, min_v, max_v);
    }
  };

  struct BatchConfig
  {
    GPUTextureMode texture_mode = GPUTextureMode::Disabled;
    GPUTransparencyMode transparency_mode = GPUTransparencyMode::Disabled;
    bool dithering = false;
    bool interlacing = false;
    bool set_mask_while_drawing = false;
    bool check_mask_before_draw = false;
    bool use_depth_buffer = false;

    // Returns the render mode for this batch.
    BatchRenderMode GetRenderMode() const
    {
      return transparency_mode == GPUTransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                                  BatchRenderMode::TransparentAndOpaque;
    }
  };

  struct BatchUBOData
  {
    u32 u_texture_window_and[2];
    u32 u_texture_window_or[2];
    float u_src_alpha_factor;
    float u_dst_alpha_factor;
    u32 u_interlaced_displayed_field;
    u32 u_set_mask_while_drawing;
  };

  struct VRAMFillUBOData
  {
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    float u_fill_color[4];
    u32 u_interlaced_displayed_field;
  };

  struct VRAMWriteUBOData
  {
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    u32 u_width;
    u32 u_height;
    u32 u_buffer_base_offset;
    u32 u_mask_or_bits;
    float u_depth_value;
  };

  struct VRAMCopyUBOData
  {
    u32 u_src_x;
    u32 u_src_y;
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    u32 u_width;
    u32 u_height;
    u32 u_set_mask_bit;
    float u_depth_value;
  };

  struct RendererStats
  {
    u32 num_batches;
    u32 num_vram_read_texture_updates;
    u32 num_uniform_buffer_updates;
  };

  class ShaderCompileProgressTracker
  {
  public:
    ShaderCompileProgressTracker(std::string title, u32 total);

    void Increment();

  private:
    std::string m_title;
    u64 m_min_time;
    u64 m_update_interval;
    u64 m_start_time;
    u64 m_last_update_time;
    u32 m_progress;
    u32 m_total;
  };

  static constexpr std::tuple<float, float, float, float> RGBA8ToFloat(u32 rgba)
  {
    return std::make_tuple(static_cast<float>(rgba & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 8) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 16) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>(rgba >> 24) * (1.0f / 255.0f));
  }

  void UpdateHWSettings(bool* framebuffer_changed, bool* shaders_changed);

  virtual void UpdateVRAMReadTexture();
  virtual void UpdateDepthBufferFromMaskBit() = 0;
  virtual void ClearDepthBuffer() = 0;
  virtual void SetScissorFromDrawingArea() = 0;
  virtual void MapBatchVertexPointer(u32 required_vertices) = 0;
  virtual void UnmapBatchVertexPointer(u32 used_vertices) = 0;
  virtual void UploadUniformBuffer(const void* uniforms, u32 uniforms_size) = 0;
  virtual void DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices) = 0;

  u32 CalculateResolutionScale() const;
  GPUDownsampleMode GetDownsampleMode(u32 resolution_scale) const;

  ALWAYS_INLINE bool IsUsingMultisampling() const { return m_multisamples > 1; }
  ALWAYS_INLINE bool IsUsingDownsampling() const
  {
    return (m_downsample_mode != GPUDownsampleMode::Disabled && !m_GPUSTAT.display_area_color_depth_24);
  }

  void SetFullVRAMDirtyRectangle()
  {
    m_vram_dirty_rect.Set(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    m_draw_mode.SetTexturePageChanged();
  }
  void ClearVRAMDirtyRectangle() { m_vram_dirty_rect.SetInvalid(); }
  void IncludeVRAMDirtyRectangle(const Common::Rectangle<u32>& rect);

  bool IsFlushed() const { return m_batch_current_vertex_ptr == m_batch_start_vertex_ptr; }

  u32 GetBatchVertexSpace() const { return static_cast<u32>(m_batch_end_vertex_ptr - m_batch_current_vertex_ptr); }
  u32 GetBatchVertexCount() const { return static_cast<u32>(m_batch_current_vertex_ptr - m_batch_start_vertex_ptr); }
  void EnsureVertexBufferSpace(u32 required_vertices);
  void EnsureVertexBufferSpaceForCurrentCommand();
  void ResetBatchVertexDepth();

  /// Returns the value to be written to the depth buffer for the current operation for mask bit emulation.
  ALWAYS_INLINE float GetCurrentNormalizedVertexDepth() const
  {
    return 1.0f - (static_cast<float>(m_current_depth) / 65535.0f);
  }

  /// Returns the interlaced mode to use when scanning out/displaying.
  ALWAYS_INLINE InterlacedRenderMode GetInterlacedRenderMode() const
  {
    if (IsInterlacedDisplayEnabled())
    {
      return m_GPUSTAT.vertical_resolution ? InterlacedRenderMode::InterleavedFields :
                                             InterlacedRenderMode::SeparateFields;
    }
    else
    {
      return InterlacedRenderMode::None;
    }
  }

  /// Returns true if the specified texture filtering mode requires dual-source blending.
  ALWAYS_INLINE bool TextureFilterRequiresDualSourceBlend(GPUTextureFilter filter)
  {
    return (filter == GPUTextureFilter::Bilinear || filter == GPUTextureFilter::JINC2 ||
            filter == GPUTextureFilter::xBR);
  }

  /// Returns true if alpha blending should be enabled for drawing the current batch.
  ALWAYS_INLINE bool UseAlphaBlending(GPUTransparencyMode transparency_mode, BatchRenderMode render_mode) const
  {
    if (m_texture_filtering == GPUTextureFilter::Bilinear || m_texture_filtering == GPUTextureFilter::JINC2 ||
        m_texture_filtering == GPUTextureFilter::xBR)
    {
      return true;
    }

    if (transparency_mode == GPUTransparencyMode::Disabled || render_mode == BatchRenderMode::OnlyOpaque)
      return false;

    return true;
  }

  /// We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
  /// on a per-pixel basis, and the opaque pixels shouldn't be blended at all.
  ALWAYS_INLINE bool NeedsTwoPassRendering() const
  {
    return (m_batch.texture_mode != GPUTextureMode::Disabled &&
            (m_batch.transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ||
             (!m_supports_dual_source_blend && m_batch.transparency_mode != GPUTransparencyMode::Disabled)));
  }

  /// Returns true if the specified VRAM fill is oversized.
  ALWAYS_INLINE static bool IsVRAMFillOversized(u32 x, u32 y, u32 width, u32 height)
  {
    return ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT);
  }

  ALWAYS_INLINE bool IsUsingSoftwareRendererForReadbacks() { return static_cast<bool>(m_sw_renderer); }

  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;
  void UpdateSoftwareRenderer(bool copy_vram_from_hw);
  void ReadSoftwareRendererVRAM(u32 x, u32 y, u32 width, u32 height);
  void UpdateSoftwareRendererVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask,
                                  bool check_mask);
  void FillSoftwareRendererVRAM(u32 x, u32 y, u32 width, u32 height, u32 color);
  void CopySoftwareRendererVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);

  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void DispatchRenderCommand() override;
  void FlushRender() override;
  void DrawRendererStats(bool is_idle_frame) override;

  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  std::tuple<s32, s32> ScaleVRAMCoordinates(s32 x, s32 y) const
  {
    return std::make_tuple(x * s32(m_resolution_scale), y * s32(m_resolution_scale));
  }

  /// Computes the area affected by a VRAM transfer, including wrap-around of X.
  Common::Rectangle<u32> GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height) const;

  /// Returns true if the VRAM copy shader should be used (oversized copies, masking).
  bool UseVRAMCopyShader(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) const;

  VRAMFillUBOData GetVRAMFillUBOData(u32 x, u32 y, u32 width, u32 height, u32 color) const;
  VRAMWriteUBOData GetVRAMWriteUBOData(u32 x, u32 y, u32 width, u32 height, u32 buffer_offset, bool set_mask,
                                       bool check_mask) const;
  VRAMCopyUBOData GetVRAMCopyUBOData(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) const;

  /// Expands a line into two triangles.
  void DrawLine(float x0, float y0, u32 col0, float x1, float y1, u32 col1, float depth);

  /// Handles quads with flipped texture coordinate directions.
  static void HandleFlippedQuadTextureCoordinates(BatchVertex* vertices);

  /// Computes polygon U/V boundaries.
  static void ComputePolygonUVLimits(BatchVertex* vertices, u32 num_vertices);

  /// Sets the depth test flag for PGXP depth buffering.
  void SetBatchDepthBuffer(bool enabled);
  void CheckForDepthClear(const BatchVertex* vertices, u32 num_vertices);

  /// UBO data for adaptive smoothing.
  struct SmoothingUBOData
  {
    float min_uv[2];
    float max_uv[2];
    float rcp_size[2];
  };

  /// Returns the number of mipmap levels used for adaptive smoothing.
  u32 GetAdaptiveDownsamplingMipLevels() const;

  /// Returns the UBO data for an adaptive smoothing pass.
  SmoothingUBOData GetSmoothingUBO(u32 level, u32 left, u32 top, u32 width, u32 height, u32 tex_width,
                                   u32 tex_height) const;

  HeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram_shadow;
  std::unique_ptr<GPU_SW_Backend> m_sw_renderer;

  BatchVertex* m_batch_start_vertex_ptr = nullptr;
  BatchVertex* m_batch_end_vertex_ptr = nullptr;
  BatchVertex* m_batch_current_vertex_ptr = nullptr;
  u32 m_batch_base_vertex = 0;
  s32 m_current_depth = 0;
  float m_last_depth_z = 1.0f;

  u32 m_resolution_scale = 1;
  u32 m_multisamples = 1;
  u32 m_max_resolution_scale = 1;
  u32 m_max_multisamples = 1;
  HostDisplay::RenderAPI m_render_api = HostDisplay::RenderAPI::None;
  bool m_true_color = true;

  union
  {
    BitField<u8, bool, 0, 1> m_supports_per_sample_shading;
    BitField<u8, bool, 1, 1> m_supports_dual_source_blend;
    BitField<u8, bool, 2, 1> m_supports_adaptive_downsampling;
    BitField<u8, bool, 3, 1> m_per_sample_shading;
    BitField<u8, bool, 4, 1> m_scaled_dithering;
    BitField<u8, bool, 5, 1> m_chroma_smoothing;

    u8 bits = 0;
  };

  GPUTextureFilter m_texture_filtering = GPUTextureFilter::Nearest;
  GPUDownsampleMode m_downsample_mode = GPUDownsampleMode::Disabled;
  bool m_using_uv_limits = false;
  bool m_pgxp_depth_buffer = false;

  BatchConfig m_batch;
  BatchUBOData m_batch_ubo_data = {};

  // Bounding box of VRAM area that the GPU has drawn into.
  Common::Rectangle<u32> m_vram_dirty_rect;

  // Statistics
  RendererStats m_renderer_stats = {};
  RendererStats m_last_renderer_stats = {};

  // Changed state
  bool m_batch_ubo_dirty = true;

private:
  enum : u32
  {
    MIN_BATCH_VERTEX_COUNT = 6,
    MAX_BATCH_VERTEX_COUNT = VERTEX_BUFFER_SIZE / sizeof(BatchVertex)
  };

  void LoadVertices();

  ALWAYS_INLINE void AddVertex(const BatchVertex& v)
  {
    std::memcpy(m_batch_current_vertex_ptr, &v, sizeof(BatchVertex));
    m_batch_current_vertex_ptr++;
  }

  template<typename... Args>
  ALWAYS_INLINE void AddNewVertex(Args&&... args)
  {
    m_batch_current_vertex_ptr->Set(std::forward<Args>(args)...);
    m_batch_current_vertex_ptr++;
  }

  void PrintSettingsToLog();
};
