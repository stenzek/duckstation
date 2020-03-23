#pragma once
#include "common/heap_array.h"
#include "gpu.h"
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class GPU_HW : public GPU
{
public:
  enum class BatchPrimitive : u8
  {
    Lines = 0,
    LineStrip = 1,
    Triangles = 2,
    TriangleStrip = 3
  };

  enum class BatchRenderMode : u8
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  GPU_HW();
  virtual ~GPU_HW();

  virtual bool IsHardwareRenderer() const override;

  virtual bool Initialize(HostDisplay* host_display, System* system, DMA* dma,
                          InterruptController* interrupt_controller, Timers* timers) override;
  virtual void Reset() override;
  virtual bool DoState(StateWrapper& sw) override;
  virtual void UpdateSettings() override;

protected:
  enum : u32
  {
    VRAM_UPDATE_TEXTURE_BUFFER_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u32),
    VERTEX_BUFFER_SIZE = 1 * 1024 * 1024,
    UNIFORM_BUFFER_SIZE = 512 * 1024
  };

  struct BatchVertex
  {
    s32 x;
    s32 y;
    u32 color;
    u32 texpage;
    u32 texcoord; // 16-bit texcoords are needed for 256 extent rectangles

    ALWAYS_INLINE void Set(s32 x_, s32 y_, u32 color_, u32 texpage_, u16 packed_texcoord)
    {
      Set(x_, y_, color_, texpage_, packed_texcoord & 0xFF, (packed_texcoord >> 8));
    }

    ALWAYS_INLINE void Set(s32 x_, s32 y_, u32 color_, u32 texpage_, u16 texcoord_x, u16 texcoord_y)
    {
      x = x_;
      y = y_;
      color = color_;
      texpage = texpage_;
      texcoord = ZeroExtend32(texcoord_x) | (ZeroExtend32(texcoord_y) << 16);
    }
  };

  struct BatchConfig
  {
    BatchPrimitive primitive;
    TextureMode texture_mode;
    TransparencyMode transparency_mode;
    bool dithering;
    bool set_mask_while_drawing;
    bool check_mask_before_draw;

    // We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
    // on a per-pixel basis, and the opaque pixels shouldn't be blended at all.
    bool NeedsTwoPassRendering() const
    {
      return transparency_mode == GPU::TransparencyMode::BackgroundMinusForeground &&
             texture_mode != TextureMode::Disabled;
    }

    // Returns the render mode for this batch.
    BatchRenderMode GetRenderMode() const
    {
      return transparency_mode == TransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                               BatchRenderMode::TransparentAndOpaque;
    }
  };

  struct BatchUBOData
  {
    u32 u_texture_window_mask[2];
    u32 u_texture_window_offset[2];
    float u_src_alpha_factor;
    float u_dst_alpha_factor;
    u32 u_set_mask_while_drawing;
    u32 padding[1];
  };

  struct RendererStats
  {
    u32 num_batches;
    u32 num_vram_read_texture_updates;
    u32 num_uniform_buffer_updates;
  };

  static constexpr std::tuple<float, float, float, float> RGBA8ToFloat(u32 rgba)
  {
    return std::make_tuple(static_cast<float>(rgba & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 8) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 16) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>(rgba >> 24) * (1.0f / 255.0f));
  }

  virtual void MapBatchVertexPointer(u32 required_vertices) = 0;
  virtual void UpdateVRAMReadTexture() = 0;

  void SetFullVRAMDirtyRectangle()
  {
    m_vram_dirty_rect.Set(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    m_draw_mode.SetTexturePageChanged();
  }
  void ClearVRAMDirtyRectangle() { m_vram_dirty_rect.SetInvalid(); }
  void IncludeVRAMDityRectangle(const Common::Rectangle<u32>& rect);

  u32 GetBatchVertexSpace() const { return static_cast<u32>(m_batch_end_vertex_ptr - m_batch_current_vertex_ptr); }
  u32 GetBatchVertexCount() const { return static_cast<u32>(m_batch_current_vertex_ptr - m_batch_start_vertex_ptr); }
  void EnsureVertexBufferSpace(u32 required_vertices);

  bool IsFlushed() const { return m_batch_current_vertex_ptr == m_batch_start_vertex_ptr; }

  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) override;
  void DrawRendererStats(bool is_idle_frame) override;

  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  std::tuple<s32, s32> ScaleVRAMCoordinates(s32 x, s32 y) const
  {
    return std::make_tuple(x * s32(m_resolution_scale), y * s32(m_resolution_scale));
  }

  /// Computes the area affected by a VRAM transfer, including wrap-around of X.
  Common::Rectangle<u32> GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height);

  HeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram_shadow;

  BatchVertex* m_batch_start_vertex_ptr = nullptr;
  BatchVertex* m_batch_end_vertex_ptr = nullptr;
  BatchVertex* m_batch_current_vertex_ptr = nullptr;
  u32 m_batch_base_vertex = 0;

  u32 m_resolution_scale = 1;
  u32 m_max_resolution_scale = 1;
  bool m_true_color = true;
  bool m_scaled_dithering = false;
  bool m_texture_filtering = false;
  bool m_supports_dual_source_blend = false;

  BatchConfig m_batch = {};
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

  static BatchPrimitive GetPrimitiveForCommand(RenderCommand rc);

  void LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr);

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
