// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <xcb/xcb.h>
#include <xcb/xproto.h>

class Error;
struct WindowInfo;

class X11Window
{
public:
  X11Window();
  X11Window(const X11Window&) = delete;
  X11Window(X11Window&& move);
  ~X11Window();

  X11Window& operator=(const X11Window&) = delete;
  X11Window& operator=(X11Window&& move);

  ALWAYS_INLINE xcb_window_t GetWindow() const { return m_window; }
  ALWAYS_INLINE xcb_window_t* GetWindowPtr() { return &m_window; }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }

  bool Create(xcb_connection_t* connection, xcb_window_t parent_window, xcb_visualid_t vi, Error* error = nullptr);
  void Destroy();

  // Setting a width/height of 0 will use parent dimensions.
  void Resize(u16 width = 0, u16 height = 0);

private:
  xcb_connection_t* m_connection = nullptr;
  xcb_window_t m_parent_window = {};
  xcb_window_t m_window = {};
  xcb_colormap_t m_colormap = {};
  u16 m_width = 0;
  u16 m_height = 0;
};

std::optional<float> GetRefreshRateFromXRandR(const WindowInfo& wi, Error* error);
