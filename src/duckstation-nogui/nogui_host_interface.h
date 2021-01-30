#pragma once
#include "common/window_info.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include "frontend-common/common_host_interface.h"
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class INISettingsInterface;

class NoGUIHostInterface : public CommonHostInterface
{
public:
  NoGUIHostInterface();
  ~NoGUIHostInterface();

  const char* GetFrontendName() const override;

  virtual bool Initialize() override;
  virtual void Shutdown() override;
  virtual void Run();

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;

  void RunLater(std::function<void()> callback) override;
  void ApplySettings(bool display_osd_messages) override;

protected:
  enum : u32
  {
    DEFAULT_WINDOW_WIDTH = 1280,
    DEFAULT_WINDOW_HEIGHT = 720
  };

  virtual void LoadSettings() override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  void UpdateInputMap() override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnRunningGameChanged() override;

  void RequestExit() override;
  virtual void PollAndUpdate() override;

  virtual bool CreatePlatformWindow() = 0;
  virtual void DestroyPlatformWindow() = 0;
  virtual std::optional<WindowInfo> GetPlatformWindowInfo() = 0;
  void OnPlatformWindowResized(u32 new_width, u32 new_height, float new_scale);

  bool CreateDisplay();
  void DestroyDisplay();
  void CreateImGuiContext();
  void RunCallbacks();

  std::unique_ptr<INISettingsInterface> m_settings_interface;
  std::deque<std::function<void()>> m_queued_callbacks;
  std::mutex m_queued_callbacks_lock;

  bool m_quit_request = false;
};
