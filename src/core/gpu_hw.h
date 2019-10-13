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

  virtual void Reset() override;

protected:
  struct HWVertex
  {
    s32 x;
    s32 y;
    u32 color;
    u32 texpage;
    u16 texcoord;
    u16 padding;

    static constexpr std::tuple<u8, u8> DecodeTexcoord(u16 texcoord)
    {
      return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
    }
    static constexpr u16 EncodeTexcoord(u8 x, u8 y) { return ZeroExtend16(x) | (ZeroExtend16(y) << 8); }
  };

  struct HWRenderBatch
  {
    enum class Primitive : u8
    {
      Lines = 0,
      LineStrip = 1,
      Triangles = 2,
      TriangleStrip = 3
    };

    u32 render_command_bits;
    Primitive primitive;
    bool transparency_enable;
    bool texture_enable;
    bool texture_blending_enable;
    TextureColorMode texture_color_mode;
    u32 texture_page_x;
    u32 texture_page_y;
    u32 texture_palette_x;
    u32 texture_palette_y;
    TransparencyMode transparency_mode;
    std::array<u8, 4> texture_window_values;

    std::vector<HWVertex> vertices;
  };

  enum class TransparencyRenderMode
  {
    Off,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  static constexpr u32 VERTEX_BUFFER_SIZE = 1 * 1024 * 1024;
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

  virtual void InvalidateVRAMReadCache();

  bool IsFlushed() const { return m_batch.vertices.empty(); }

  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) override;

  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  std::tuple<s32, s32> ScaleVRAMCoordinates(s32 x, s32 y) const
  {
    return std::make_tuple(x * s32(m_resolution_scale), y * s32(m_resolution_scale));
  }

  std::string GenerateVertexShader(bool textured);
  std::string GenerateFragmentShader(TransparencyRenderMode transparency, bool textured,
                                     TextureColorMode texture_color_mode, bool blending);
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateRGB24DecodeFragmentShader();

  u32 m_resolution_scale = 1;
  HWRenderBatch m_batch = {};

private:
  static HWRenderBatch::Primitive GetPrimitiveForCommand(RenderCommand rc);

  void GenerateShaderHeader(std::stringstream& ss);

  void LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr);
};
