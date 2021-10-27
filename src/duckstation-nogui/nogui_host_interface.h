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

  void ReportMessage(const char* message) override;
  void ReportError(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  void RunLater(std::function<void()> callback) override;

  virtual void OnDisplayInvalidated() override;
  virtual void OnSystemPerformanceCountersUpdated() override;

protected:
  enum : u32
  {
    DEFAULT_WINDOW_WIDTH = 1280,
    DEFAULT_WINDOW_HEIGHT = 720
  };

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  void RequestExit() override;

  virtual void SetDefaultSettings(SettingsInterface& si) override;

  virtual bool CreatePlatformWindow() = 0;
  virtual void DestroyPlatformWindow() = 0;
  virtual std::optional<WindowInfo> GetPlatformWindowInfo() = 0;

  bool CreateDisplay(bool fullscreen);
  void DestroyDisplay();
  void RunCallbacks();

  std::deque<std::function<void()>> m_queued_callbacks;
  std::mutex m_queued_callbacks_lock;

  bool m_quit_request = false;
};
