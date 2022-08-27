#pragma once

#include "nogui_platform.h"

// Why do we have all these here instead of in the source?
// Because X11 is a giant turd and #defines commonly used words.
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "nogui_host.h"
#include "nogui_platform.h"

#include <atomic>
#include <deque>
#include <linux/input-event-codes.h>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// Include X stuff *last*.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD 1
#define _NET_WM_STATE_TOGGLE 2

class X11NoGUIPlatform : public NoGUIPlatform
{
public:
  X11NoGUIPlatform();
  ~X11NoGUIPlatform();

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
  void InitializeKeyMap();
  void SaveWindowGeometry();
  void ProcessXEvents();

  std::atomic_bool m_message_loop_running{false};
  std::atomic_bool m_fullscreen{false};

  WindowInfo m_window_info = {};

  Display* m_display = nullptr;
  Window m_window = {};

  std::unordered_map<s32, std::string> m_key_map;

  std::deque<std::function<void()>> m_callback_queue;
  std::mutex m_callback_queue_mutex;
};

class XDisplayLocker
{
public:
  XDisplayLocker(Display* dpy) : m_display(dpy) { XLockDisplay(m_display); }

  ~XDisplayLocker() { XUnlockDisplay(m_display); }

private:
  Display* m_display;
};
