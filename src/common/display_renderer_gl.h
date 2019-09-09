#pragma once
#include "display_renderer.h"
#include <memory>
#include <mutex>

class DisplayRendererGL final : public DisplayRenderer
{
public:
  DisplayRendererGL(WindowHandleType window_handle, u32 window_width, u32 window_height);
  ~DisplayRendererGL();

  BackendType GetBackendType() override;

  std::unique_ptr<Display> CreateDisplay(const char* name, Display::Type type,
                                         u8 priority = Display::DEFAULT_PRIORITY) override;

  void WindowResized(u32 window_width, u32 window_height) override;

  bool BeginFrame() override;
  void RenderDisplays() override;
  void EndFrame() override;

protected:
  bool Initialize() override;

private:
  bool CreateQuadVAO();
  void BindQuadVAO();

  bool CreateQuadProgram();

  u32 m_quad_vbo_id = 0;
  u32 m_quad_vao_id = 0;
  u32 m_quad_program_id = 0;
};
