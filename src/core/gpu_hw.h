#pragma once
#include "gpu.h"
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

class GPU_HW : public GPU
{
public:
  GPU_HW();
  virtual ~GPU_HW();

  virtual bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers) override;
  virtual void Reset() override;
  virtual void UpdateSettings() override;

protected:
  enum class HWPrimitive : u8
  {
    Lines = 0,
    LineStrip = 1,
    Triangles = 2,
    TriangleStrip = 3
  };

  enum class HWBatchRenderMode : u8
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  struct HWVertex
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

  struct HWBatchConfig
  {
    u32 texture_page_x;
    u32 texture_page_y;
    u32 texture_palette_x;
    u32 texture_palette_y;
    HWPrimitive primitive;
    TextureMode texture_mode;
    TransparencyMode transparency_mode;
    std::array<u8, 4> texture_window_values;
    bool dithering;

    // We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
    // on a per-pixel basis, and the opaque pixels shouldn't be blended at all.
    bool NeedsTwoPassRendering() const
    {
      return transparency_mode == GPU::TransparencyMode::BackgroundMinusForeground &&
             texture_mode != TextureMode::Disabled;
    }

    // Returns the render mode for this batch.
    HWBatchRenderMode GetRenderMode() const
    {
      return transparency_mode == TransparencyMode::Disabled ? HWBatchRenderMode::TransparencyDisabled :
                                                               HWBatchRenderMode::TransparentAndOpaque;
    }
  };

  static constexpr u32 VRAM_UPDATE_TEXTURE_BUFFER_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u32);
  static constexpr u32 VERTEX_BUFFER_SIZE = 1 * 1024 * 1024;
  static constexpr u32 MIN_BATCH_VERTEX_COUNT = 6;
  static constexpr u32 MAX_BATCH_VERTEX_COUNT = VERTEX_BUFFER_SIZE / sizeof(HWVertex);
  static constexpr u32 TEXTURE_TILE_SIZE = 256;
  static constexpr u32 TEXTURE_TILE_X_COUNT = VRAM_WIDTH / TEXTURE_TILE_SIZE;
  static constexpr u32 TEXTURE_TILE_Y_COUNT = VRAM_HEIGHT / TEXTURE_TILE_SIZE;
  static constexpr u32 TEXTURE_TILE_COUNT = TEXTURE_TILE_X_COUNT * TEXTURE_TILE_Y_COUNT;

  static constexpr std::tuple<float, float, float, float> RGBA8ToFloat(u32 rgba)
  {
    return std::make_tuple(static_cast<float>(rgba & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 16) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 8) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>(rgba >> 24) * (1.0f / 255.0f));
  }

  virtual void InvalidateVRAMReadCache() = 0;

  virtual void MapBatchVertexPointer(u32 required_vertices) = 0;

  u32 GetBatchVertexSpace() const { return static_cast<u32>(m_batch_end_vertex_ptr - m_batch_current_vertex_ptr); }
  u32 GetBatchVertexCount() const { return static_cast<u32>(m_batch_current_vertex_ptr - m_batch_start_vertex_ptr); }

  bool IsFlushed() const { return m_batch_current_vertex_ptr == m_batch_start_vertex_ptr; }

  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) override;

  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  std::tuple<s32, s32> ScaleVRAMCoordinates(s32 x, s32 y) const
  {
    return std::make_tuple(x * s32(m_resolution_scale), y * s32(m_resolution_scale));
  }

  std::string GenerateVertexShader(bool textured);
  std::string GenerateFragmentShader(HWBatchRenderMode transparency, TextureMode texture_mode, bool dithering);
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced);
  std::string GenerateVRAMWriteFragmentShader();

  HWBatchConfig m_batch = {};

  HWVertex* m_batch_start_vertex_ptr = nullptr;
  HWVertex* m_batch_end_vertex_ptr = nullptr;
  HWVertex* m_batch_current_vertex_ptr = nullptr;
  u32 m_batch_base_vertex = 0;

  u32 m_resolution_scale = 1;
  u32 m_max_resolution_scale = 1;
  bool m_true_color = false;

private:
  static HWPrimitive GetPrimitiveForCommand(RenderCommand rc);

  void GenerateShaderHeader(std::stringstream& ss);

  void LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr);
  void AddDuplicateVertex();
};
