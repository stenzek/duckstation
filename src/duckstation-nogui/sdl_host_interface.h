#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include "frontend-common/common_host_interface.h"
#include <SDL.h>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class AudioStream;

class INISettingsInterface;

struct GameListEntry;

class SDLHostInterface final : public CommonHostInterface
{
public:
  SDLHostInterface();
  ~SDLHostInterface();

  static std::unique_ptr<SDLHostInterface> Create();

  const char* GetFrontendName() const override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  bool Initialize() override;
  void Shutdown() override;

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;

  bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) override;

  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

  void RunLater(std::function<void()> callback) override;
  void ApplySettings(bool display_osd_messages) override;

  void Run();

protected:
  void LoadSettings() override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnRunningGameChanged() override;

  void RequestExit() override;
  void PollAndUpdate() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;
  void UpdateInputMap() override;

private:
  bool CreateSDLWindow();
  void DestroySDLWindow();
  bool CreateDisplay();
  void DestroyDisplay();
  void CreateImGuiContext();
  void UpdateFramebufferScale();

  void HandleSDLEvent(const SDL_Event* event);
  void ProcessEvents();

  SDL_Window* m_window = nullptr;
  std::unique_ptr<INISettingsInterface> m_settings_interface;
  u32 m_run_later_event_id = 0;

  bool m_fullscreen = false;
  bool m_quit_request = false;
};
