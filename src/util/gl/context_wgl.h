// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "context.h"

#include "../opengl_loader.h"

#include "common/windows_headers.h"

#include "glad_wgl.h"

#include <optional>

namespace GL {

class ContextWGL final : public Context
{
public:
  ContextWGL(const WindowInfo& wi);
  ~ContextWGL() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

  void* GetProcAddress(const char* name) override;
  bool ChangeSurface(const WindowInfo& new_wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool IsCurrent() override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SetSwapInterval(s32 interval) override;
  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

private:
  ALWAYS_INLINE HWND GetHWND() const { return static_cast<HWND>(m_wi.window_handle); }

  HDC GetDCAndSetPixelFormat(HWND hwnd);

  bool Initialize(std::span<const Version> versions_to_try, Error* error);
  bool InitializeDC();
  void ReleaseDC();
  bool CreatePBuffer();
  bool CreateAnyContext(HGLRC share_context, bool make_current);
  bool CreateVersionContext(const Version& version, HGLRC share_context, bool make_current);

  HDC m_dc = {};
  HGLRC m_rc = {};

  // Can't change pixel format once it's set for a RC.
  std::optional<int> m_pixel_format;

  // Dummy window for creating a PBuffer off when we're surfaceless.
  HWND m_dummy_window = {};
  HDC m_dummy_dc = {};
  HPBUFFERARB m_pbuffer = {};
};

} // namespace GL