// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "window_info.h"

#include "common/types.h"

#include <memory>
#include <span>

class Error;

class OpenGLContext
{
public:
  OpenGLContext();
  virtual ~OpenGLContext();

  using SurfaceHandle = void*;
  static constexpr SurfaceHandle MAIN_SURFACE = nullptr;

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

  ALWAYS_INLINE bool IsGLES() const { return (m_version.profile == Profile::ES); }

  virtual void* GetProcAddress(const char* name) = 0;
  virtual SurfaceHandle CreateSurface(WindowInfo& wi, Error* error = nullptr) = 0;
  virtual void DestroySurface(SurfaceHandle handle) = 0;
  virtual void ResizeSurface(WindowInfo& wi, SurfaceHandle handle) = 0;
  virtual bool SwapBuffers() = 0;
  virtual bool IsCurrent() const = 0;
  virtual bool MakeCurrent(SurfaceHandle surface, Error* error = nullptr) = 0;
  virtual bool DoneCurrent() = 0;
  virtual bool SupportsNegativeSwapInterval() const = 0;
  virtual bool SetSwapInterval(s32 interval, Error* error = nullptr) = 0;
  virtual std::unique_ptr<OpenGLContext> CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface, Error* error) = 0;

  static std::unique_ptr<OpenGLContext> Create(WindowInfo& wi, SurfaceHandle* surface, bool prefer_gles_context,
                                               Error* error);

protected:
  Version m_version = {};
};
