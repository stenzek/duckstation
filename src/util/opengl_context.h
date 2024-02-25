// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "window_info.h"

#include "common/types.h"

#include <memory>
#include <span>
#include <vector>

class Error;

class OpenGLContext
{
public:
  OpenGLContext(const WindowInfo& wi);
  virtual ~OpenGLContext();

  enum class Profile
  {
    NoProfile,
    Core,
    ES
  };

  struct Version
  {
    Profile profile;
    int major_version;
    int minor_version;
  };

  struct FullscreenModeInfo
  {
    u32 width;
    u32 height;
    float refresh_rate;
  };

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_wi; }
  ALWAYS_INLINE bool IsGLES() const { return (m_version.profile == Profile::ES); }
  ALWAYS_INLINE u32 GetSurfaceWidth() const { return m_wi.surface_width; }
  ALWAYS_INLINE u32 GetSurfaceHeight() const { return m_wi.surface_height; }
  ALWAYS_INLINE GPUTexture::Format GetSurfaceFormat() const { return m_wi.surface_format; }

  virtual void* GetProcAddress(const char* name) = 0;
  virtual bool ChangeSurface(const WindowInfo& new_wi) = 0;
  virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) = 0;
  virtual bool SwapBuffers() = 0;
  virtual bool IsCurrent() = 0;
  virtual bool MakeCurrent() = 0;
  virtual bool DoneCurrent() = 0;
  virtual bool SetSwapInterval(s32 interval) = 0;
  virtual std::unique_ptr<OpenGLContext> CreateSharedContext(const WindowInfo& wi, Error* error) = 0;

  virtual std::vector<FullscreenModeInfo> EnumerateFullscreenModes();

  static std::unique_ptr<OpenGLContext> Create(const WindowInfo& wi, Error* error);

protected:
  WindowInfo m_wi;
  Version m_version = {};
};
