#pragma once
#include "common/gl_program.h"
#include "common/gl_texture.h"
#include "glad.h"
#include "gpu_hw.h"
#include <array>
#include <memory>
#include <tuple>

class GPU_HW_OpenGL : public GPU_HW
{
public:
  GPU_HW_OpenGL();
  ~GPU_HW_OpenGL() override;

  bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers) override;
  void Reset() override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;

  void DrawDebugWindows() override;
  void DrawDebugMenu() override;
  void UpdateSettings() override;

protected:
  void UpdateDisplay() override;
  void UpdateDrawingArea() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void InvalidateVRAMReadCache() override;

private:
  struct GLStats
  {
    u32 num_vram_read_texture_updates;
    u32 num_batches;
    u32 num_vertices;
  };

  void DrawRendererStatistics();

  std::tuple<s32, s32> ConvertToFramebufferCoordinates(s32 x, s32 y);

  void SetMaxResolutionScale();
  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();
  void UpdateVRAMReadTexture();

  void CreateVertexBuffer();

  bool CompilePrograms();
  bool CompileProgram(GL::Program& prog, TransparencyRenderMode transparent, bool textured,
                      TextureColorMode texture_color_mode, bool blending);
  void SetDrawState(TransparencyRenderMode render_mode);

  // downsample texture - used for readbacks at >1xIR.
  std::unique_ptr<GL::Texture> m_vram_texture;
  std::unique_ptr<GL::Texture> m_vram_read_texture;
  std::unique_ptr<GL::Texture> m_vram_downsample_texture;
  std::unique_ptr<GL::Texture> m_display_texture;

  GLuint m_vram_fbo = 0;
  GLuint m_vram_read_fbo = 0;
  GLuint m_vram_downsample_fbo = 0;
  GLuint m_display_fbo = 0;

  GLuint m_vertex_buffer = 0;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;

  bool m_vram_read_texture_dirty = true;
  bool m_drawing_area_changed = true;
  bool m_show_renderer_statistics = false;

  std::array<std::array<std::array<std::array<GL::Program, 2>, 3>, 2>, 4> m_render_programs;
  GL::Program m_reinterpret_rgb8_program;

  GLStats m_stats = {};
  GLStats m_last_stats = {};
};
