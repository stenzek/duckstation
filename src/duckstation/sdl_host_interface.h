#pragma once
#include "YBaseLib/String.h"
#include "YBaseLib/Timer.h"
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_interface.h"
#include "core/host_display.h"
#include <SDL.h>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

class System;
class DigitalController;
class MemoryCard;
class AudioStream;

class SDLHostInterface : public HostInterface
{
public:
  SDLHostInterface();
  ~SDLHostInterface();

  static std::unique_ptr<SDLHostInterface> Create(const char* filename = nullptr, const char* exp1_filename = nullptr,
                                              const char* save_state_filename = nullptr);

  static TinyString GetSaveStateFilename(u32 index);

  void ReportMessage(const char* message) override;

  // Adds OSD messages, duration is in seconds.
  void AddOSDMessage(const char* message, float duration = 2.0f) override;

  void Run();

private:
  static constexpr u32 NUM_QUICK_SAVE_STATES = 10;
  static constexpr char RESUME_SAVESTATE_FILENAME[] = "savestate_resume.bin";

  struct OSDMessage
  {
    String text;
    Timer time;
    float duration;
  };

  bool HasSystem() const { return static_cast<bool>(m_system); }

#ifdef WIN32
  bool UseOpenGLRenderer() const { return m_settings.gpu_renderer == Settings::GPURenderer::HardwareOpenGL; }
#else
  bool UseOpenGLRenderer() const { return true; }
#endif

  bool CreateSDLWindow();
  bool CreateDisplay();
  void CreateImGuiContext();
  bool CreateAudioStream();

  void OpenGameControllers();
  void CloseGameControllers();

  void SaveSettings();

  bool InitializeSystem(const char* filename = nullptr, const char* exp1_filename = nullptr);
  void ConnectDevices();
  void ResetPerformanceCounters();
  void SwitchGPURenderer();
  void UpdateFullscreen();

  // We only pass mouse input through if it's grabbed
  void DrawImGui();
  void DoReset();
  void DoPowerOff();
  void DoResume();
  void DoStartDisc();
  void DoStartBIOS();
  void DoChangeDisc();
  void DoLoadState(u32 index);
  void DoSaveState(u32 index);
  void DoTogglePause();
  void DoFrameStep();
  void DoToggleSoftwareRendering();
  void DoToggleFullscreen();
  void DoModifyInternalResolution(s32 increment);

  void HandleSDLEvent(const SDL_Event* event);
  void HandleSDLKeyEvent(const SDL_Event* event);
  void ClearImGuiFocus();

  void DrawMainMenuBar();
  void DrawQuickSettingsMenu();
  void DrawDebugMenu();
  void DrawPoweredOffWindow();
  void DrawSettingsWindow();
  void DrawAboutWindow();
  void DrawOSDMessages();
  void DrawDebugWindows();
  bool DrawFileChooser(const char* label, std::string* path, const char* filter = nullptr);

  SDL_Window* m_window = nullptr;
  std::unique_ptr<HostDisplayTexture> m_app_icon_texture;

  std::string m_settings_filename;

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;

  std::map<int, SDL_GameController*> m_sdl_controllers;

  std::shared_ptr<DigitalController> m_controller;

  float m_vps = 0.0f;
  float m_fps = 0.0f;
  float m_speed = 0.0f;
  u32 m_last_frame_number = 0;
  u32 m_last_internal_frame_number = 0;
  u32 m_last_global_tick_counter = 0;
  Timer m_fps_timer;

  bool m_quit_request = false;
  bool m_frame_step_request = false;
  bool m_focus_main_menu_bar = false;
  bool m_settings_window_open = false;
  bool m_about_window_open = false;
};
