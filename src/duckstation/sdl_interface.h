#pragma once
#include "YBaseLib/String.h"
#include "YBaseLib/Timer.h"
#include "common/gl_program.h"
#include "common/gl_texture.h"
#include "core/host_interface.h"
#include <SDL.h>
#include <array>
#include <deque>
#include <memory>
#include <mutex>

class System;
class DigitalController;
class MemoryCard;
class AudioStream;

class SDLInterface : public HostInterface
{
public:
  SDLInterface();
  ~SDLInterface();

  static std::unique_ptr<SDLInterface> Create(const char* filename = nullptr, const char* exp1_filename = nullptr,
                                              const char* save_state_filename = nullptr);

  static TinyString GetSaveStateFilename(u32 index);

  void SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height,
                         float aspect_ratio) override;

  void ReportMessage(const char* message) override;

  // Adds OSD messages, duration is in seconds.
  void AddOSDMessage(const char* message, float duration = 2.0f) override;

  void Run();

private:
  static constexpr u32 NUM_QUICK_SAVE_STATES = 10;

  struct OSDMessage
  {
    String text;
    Timer time;
    float duration;
  };

  bool HasSystem() const { return static_cast<bool>(m_system); }

  bool CreateSDLWindow();
  bool CreateGLContext();
  bool CreateImGuiContext();
  bool CreateGLResources();
  bool CreateAudioStream();
  void UpdateAudioVisualSync();

  bool InitializeSystem(const char* filename = nullptr, const char* exp1_filename = nullptr);
  void ConnectDevices();

  // We only pass mouse input through if it's grabbed
  bool IsWindowFullscreen() const;
  void DrawImGui();
  void DoReset();
  void DoPowerOff();
  void DoResume();
  void DoStartDisc();
  void DoStartBIOS();
  void DoLoadState(u32 index);
  void DoSaveState(u32 index);

  bool HandleSDLEvent(const SDL_Event* event);
  bool HandleSDLKeyEvent(const SDL_Event* event);
  bool PassEventToImGui(const SDL_Event* event);
  void Render();
  void RenderDisplay();
  void DrawMainMenuBar();
  void DrawPoweredOffWindow();
  void DrawAboutWindow();
  void DrawOSDMessages();

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_gl_context = nullptr;
  int m_window_width = 0;
  int m_window_height = 0;

  std::unique_ptr<GL::Texture> m_app_icon_texture = nullptr;

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GL::Texture* m_display_texture = nullptr;
  u32 m_display_texture_offset_x = 0;
  u32 m_display_texture_offset_y = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  float m_display_aspect_ratio = 1.0f;
  bool m_display_texture_changed = false;

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;

  std::shared_ptr<DigitalController> m_controller;
  std::shared_ptr<MemoryCard> m_memory_card;

  float m_vps = 0.0f;
  float m_fps = 0.0f;
  float m_speed = 0.0f;
  u32 m_last_frame_number = 0;
  u32 m_last_internal_frame_number = 0;
  u32 m_last_global_tick_counter = 0;
  Timer m_fps_timer;

  bool m_about_window_open = false;
  bool m_speed_limiter_enabled = true;
  bool m_speed_limiter_temp_disabled = false;
};
