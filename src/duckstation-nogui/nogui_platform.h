#pragma once

#include "common/types.h"
#include "core/host_display.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class SettingsInterface;

class NoGUIPlatform
{
public:
  virtual ~NoGUIPlatform() = default;

  virtual void ReportError(const std::string_view& title, const std::string_view& message) = 0;
  virtual bool ConfirmMessage(const std::string_view& title, const std::string_view& message) = 0;

  virtual void SetDefaultConfig(SettingsInterface& si) = 0;

  virtual bool CreatePlatformWindow(std::string title) = 0;
  virtual void DestroyPlatformWindow() = 0;

  virtual std::optional<WindowInfo> GetPlatformWindowInfo() = 0;
  virtual void SetPlatformWindowTitle(std::string title) = 0;
  virtual void* GetPlatformWindowHandle() = 0;

  virtual std::optional<u32> ConvertHostKeyboardStringToCode(const std::string_view& str) = 0;
  virtual std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code) = 0;

  virtual void RunMessageLoop() = 0;
  virtual void ExecuteInMessageLoop(std::function<void()> func) = 0;
  virtual void QuitMessageLoop() = 0;

  virtual void SetFullscreen(bool enabled) = 0;

  virtual bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) = 0;

  virtual bool OpenURL(const std::string_view& url) = 0;
  virtual bool CopyTextToClipboard(const std::string_view& text) = 0;

#ifdef _WIN32
  static std::unique_ptr<NoGUIPlatform> CreateWin32Platform();
#endif

#ifdef NOGUI_PLATFORM_WAYLAND
  static std::unique_ptr<NoGUIPlatform> CreateWaylandPlatform();
#endif
#ifdef NOGUI_PLATFORM_X11
  static std::unique_ptr<NoGUIPlatform> CreateX11Platform();
#endif
#ifdef NOGUI_PLATFORM_VTY
  static std::unique_ptr<NoGUIPlatform> CreateVTYPlatform();
#endif

protected:
  static constexpr s32 DEFAULT_WINDOW_WIDTH = 1280;
  static constexpr s32 DEFAULT_WINDOW_HEIGHT = 720;
};

extern std::unique_ptr<NoGUIPlatform> g_nogui_window;