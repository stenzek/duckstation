#pragma once
#include "common/display_renderer.h"
#include "pse-sdl/sdl_audio_mixer.h"
#include <array>
#include <deque>
#include <mutex>

struct SDL_Window;
union SDL_Event;

class System;

class SDLInterface
{
public:
  using MixerType = SDLAudioMixer;
  // using MixerType = Audio::NullMixer;

  SDLInterface(SDL_Window* window, std::unique_ptr<DisplayRenderer> display_renderer,
                   std::unique_ptr<MixerType> mixer);
  ~SDLInterface();

  static std::unique_ptr<SDLInterface>
  Create(DisplayRenderer::BackendType display_renderer_backend = DisplayRenderer::GetDefaultBackendType());

  static TinyString GetSaveStateFilename(u32 index);

  bool InitializeSystem(const char* filename, s32 save_state_index = -1);

  DisplayRenderer* GetDisplayRenderer() const { return m_display_renderer.get(); }
  Audio::Mixer* GetAudioMixer() const { return m_mixer.get(); }

  void ReportMessage(const char* message);

  void Run();

  // Adds OSD messages, duration is in seconds.
  void AddOSDMessage(const char* message, float duration = 2.0f);

protected:
  struct OSDMessage
  {
    String text;
    Timer time;
    float duration;
  };

  // We only pass mouse input through if it's grabbed
  bool IsWindowFullscreen() const;
  void RenderImGui();
  void DoLoadState(u32 index);
  void DoSaveState(u32 index);

  bool HandleSDLEvent(const SDL_Event* event);
  bool PassEventToImGui(const SDL_Event* event);
  void Render();
  void RenderMainMenuBar();
  void RenderOSDMessages();

  SDL_Window* m_window;

  std::unique_ptr<DisplayRenderer> m_display_renderer;
  std::unique_ptr<MixerType> m_mixer;
  std::unique_ptr<System> m_system;

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;

  bool m_running = false;
};
