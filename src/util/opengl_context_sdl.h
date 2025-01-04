// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context.h"
#include "opengl_loader.h"

#include <optional>

typedef struct SDL_GLContextState* SDL_GLContext;
struct SDL_Window;

class OpenGLContextSDL final : public OpenGLContext
{
public:
  OpenGLContextSDL();
  ~OpenGLContextSDL() override;

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
  bool Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try, bool share_context,
                  Error* error);

  bool CreateVersionContext(const Version& version, SDL_Window* window, GPUTexture::Format surface_format,
                            bool share_context, bool make_current);

  void UpdateWindowInfoSize(WindowInfo& wi, SDL_Window* window) const;

  SDL_GLContext m_context = nullptr;
  SDL_Window* m_current_window = nullptr;
};
