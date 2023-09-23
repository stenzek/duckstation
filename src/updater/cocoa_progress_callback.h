// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/progress_callback.h"

#include <AppKit/AppKit.h>
#include <Cocoa/Cocoa.h>

#ifndef __OBJC__
#error This file needs to be compiled with Objective C++.
#endif

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

class CocoaProgressCallback final : public BaseProgressCallback
{
public:
  CocoaProgressCallback();
  ~CocoaProgressCallback();

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const char* title) override;
  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;

  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  void ModalInformation(const char* message) override;
  
private:
  enum : int
  {
    WINDOW_WIDTH = 600,
    WINDOW_HEIGHT = 300,
    WINDOW_MARGIN = 20,
    SUBWINDOW_PADDING = 10,
    SUBWINDOW_WIDTH = WINDOW_WIDTH - WINDOW_MARGIN - WINDOW_MARGIN,
  };

  bool Create();
  void Destroy();
  void UpdateProgress();
  void AppendMessage(const char* message);

  NSWindow* m_window = nil;
  NSView* m_view = nil;
  NSTextField* m_status = nil;
  NSProgressIndicator* m_progress = nil;
  NSScrollView* m_text_scroll = nil;
  NSTextView* m_text = nil;
};
