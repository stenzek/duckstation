// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context.h"
#include "opengl_loader.h"

#include "common/windows_headers.h"

#include "glad/wgl.h"

#include <optional>

class OpenGLContextWGL final : public OpenGLContext
{
public:
  OpenGLContextWGL();
  ~OpenGLContextWGL() override;

  static std::unique_ptr<OpenGLContext> Create(WindowInfo& wi, SurfaceHandle* surface,
                                               std::span<const Version> versions_to_try, Error* error);

  void* GetProcAddress(const char* name) override;
  SurfaceHandle CreateSurface(WindowInfo& wi, Error* error = nullptr) override;
  void DestroySurface(SurfaceHandle handle) override;
  void ResizeSurface(WindowInfo& wi, SurfaceHandle handle) override;
  bool SwapBuffers() override;
  bool IsCurrent() const override;
  bool MakeCurrent(SurfaceHandle surface, Error* error = nullptr) override;
  bool DoneCurrent() override;
  bool SupportsNegativeSwapInterval() const override;
  bool SetSwapInterval(s32 interval, Error* error = nullptr) override;
  std::unique_ptr<OpenGLContext> CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface, Error* error) override;

private:
  bool Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try, Error* error);

  bool CreateAnyContext(HDC hdc, HGLRC share_context, bool make_current, Error* error);
  bool CreateVersionContext(const Version& version, HDC hdc, HGLRC share_context, bool make_current, Error* error);

  HDC CreateDCAndSetPixelFormat(WindowInfo& wi, Error* error);
  HDC GetPBufferDC(Error* error);

  HDC m_current_dc = {};
  HGLRC m_rc = {};

  // Can't change pixel format once it's set for a RC.
  std::optional<int> m_pixel_format;

  // Dummy window for creating a PBuffer off when we're surfaceless.
  HWND m_dummy_window = {};
  HDC m_dummy_dc = {};
  HPBUFFERARB m_pbuffer = {};
  HDC m_pbuffer_dc = {};
};
