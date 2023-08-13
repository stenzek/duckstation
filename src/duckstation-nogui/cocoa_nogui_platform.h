// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <AppKit/AppKit.h>
#include <Cocoa/Cocoa.h>

#ifndef __OBJC__
#error This file needs to be compiled with Objective C++.
#endif

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

#include "nogui_platform.h"

#include <atomic>

@interface CocoaNoGUIView : NSView<NSWindowDelegate>
- (BOOL)acceptsFirstResponder;
- (BOOL)canBecomeKeyView;
- (void)mouseDown:(NSEvent *)event;
- (void)rightMouseDown:(NSEvent *)event;
- (void)otherMouseDown:(NSEvent *)event;
- (void)mouseUp:(NSEvent *)event;
- (void)rightMouseUp:(NSEvent *)event;
- (void)otherMouseUp:(NSEvent *)event;
- (void)mouseMoved:(NSEvent *)event;
- (void)keyDown:(NSEvent *)event;
- (void)keyUp:(NSEvent *)event;
- (void)windowDidEndLiveResize:(NSNotification *)notif;
@end

class CocoaNoGUIPlatform : public NoGUIPlatform
{
public:
  CocoaNoGUIPlatform();
  ~CocoaNoGUIPlatform();

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
  NSWindow* m_window = nil;
  float m_window_scale = 1.0f;

  std::atomic_bool m_fullscreen{false};
};
