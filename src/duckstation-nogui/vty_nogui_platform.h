#pragma once
#include "nogui_platform.h"
#include <atomic>
#include <deque>
#include <functional>
#include <libevdev/libevdev.h>
#include <mutex>
#include <vector>

class VTYNoGUIPlatform : public NoGUIPlatform
{
public:
  VTYNoGUIPlatform();
  ~VTYNoGUIPlatform();

  bool Initialize();

  void ReportError(const std::string_view& title, const std::string_view& message) override;
  bool ConfirmMessage(const std::string_view& title, const std::string_view& message) override;

  void SetDefaultConfig(SettingsInterface& si) override;

  bool CreatePlatformWindow(std::string title) override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;
  void SetPlatformWindowTitle(std::string title) override;
  void* GetPlatformWindowHandle() override;

  std::optional<u32> ConvertHostKeyboardStringToCode(const std::string_view& str) override;
  std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code) override;

  void RunMessageLoop() override;
  void ExecuteInMessageLoop(std::function<void()> func) override;
  void QuitMessageLoop() override;

  void SetFullscreen(bool enabled) override;

  bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) override;

  bool OpenURL(const std::string_view& url) override;
  bool CopyTextToClipboard(const std::string_view& text) override;

private:
  void OpenEVDevFDs();
  void CloseEVDevFDs();
  void PollEvDevKeyboards();
  void SetImGuiKeyMap();

  struct EvDevKeyboard
  {
    struct libevdev* obj;
    int fd;
  };

  std::vector<EvDevKeyboard> m_evdev_keyboards;

  std::deque<std::function<void()>> m_callback_queue;
  std::mutex m_callback_queue_mutex;

  std::atomic_bool m_message_loop_running{false};
};
