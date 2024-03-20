// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <atomic>

#include "common/windows_headers.h"

#include "nogui_platform.h"

#include <SDL.h>

class SDLNoGUIPlatform : public NoGUIPlatform
{
public:
  SDLNoGUIPlatform();
  ~SDLNoGUIPlatform();

  bool Initialize();

  void ReportError(const std::string_view& title, const std::string_view& message) override;
  bool ConfirmMessage(const std::string_view& title, const std::string_view& message) override;

  void SetDefaultConfig(SettingsInterface& si) override;

  bool CreatePlatformWindow(std::string title) override;
  bool HasPlatformWindow() const override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;
  void SetPlatformWindowTitle(std::string title) override;

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
  void ProcessEvent(const SDL_Event* ev);

  SDL_Window* m_window = nullptr;
  float m_window_scale = 1.0f;
  u32 m_func_event_id = 0;
  u32 m_wakeup_event_id = 0;

  std::atomic_bool m_message_loop_running{false};
  std::atomic_bool m_fullscreen{false};
};