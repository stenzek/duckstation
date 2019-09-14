#pragma once
#include "YBaseLib/String.h"
#include "YBaseLib/Timer.h"
#include "common/gl_program.h"
#include "common/gl_texture.h"
#include "pse/host_interface.h"
#include <SDL.h>
#include <array>
#include <deque>
#include <mutex>

class System;

class SDLInterface : public HostInterface
{
public:
  SDLInterface();
  ~SDLInterface();

  static std::unique_ptr<SDLInterface> Create();

  static TinyString GetSaveStateFilename(u32 index);

  void SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height) override;

  void ReportMessage(const char* message) override;

  // Adds OSD messages, duration is in seconds.
  void AddOSDMessage(const char* message, float duration = 2.0f) override;

  void Run();

private:
  struct OSDMessage
  {
    String text;
    Timer time;
    float duration;
  };

  bool CreateSDLWindow();
  bool CreateGLContext();
  bool CreateImGuiContext();
  bool CreateGLResources();

  // We only pass mouse input through if it's grabbed
  bool IsWindowFullscreen() const;
  void RenderImGui();
  void DoLoadState(u32 index);
  void DoSaveState(u32 index);

  bool HandleSDLEvent(const SDL_Event* event);
  bool PassEventToImGui(const SDL_Event* event);
  void Render();
  void RenderDisplay();
  void RenderMainMenuBar();
  void RenderOSDMessages();

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_gl_context = nullptr;
  int m_window_width = 0;
  int m_window_height = 0;

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GL::Texture* m_display_texture = nullptr;
  u32 m_display_texture_offset_x = 0;
  u32 m_display_texture_offset_y = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  bool m_display_texture_changed = false;

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;
};
