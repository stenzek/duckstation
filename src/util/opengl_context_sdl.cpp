// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_sdl.h"
#include "opengl_loader.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/scoped_guard.h"

#include "SDL.h"

LOG_CHANNEL(GPUDevice);

OpenGLContextSDL::OpenGLContextSDL() = default;

OpenGLContextSDL::~OpenGLContextSDL()
{
  if (SDL_GL_GetCurrentContext() == m_context)
    SDL_GL_MakeCurrent(nullptr, nullptr);

  if (m_context)
    SDL_GL_DeleteContext(m_context);
}

std::unique_ptr<OpenGLContext> OpenGLContextSDL::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                        std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextSDL> context = std::make_unique<OpenGLContextSDL>();
  if (!context->Initialize(wi, surface, versions_to_try, false, error))
    context.reset();

  return context;
}

bool OpenGLContextSDL::Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try,
                                  bool share_context, Error* error)
{
  static bool opengl_loaded = false;
  if (!opengl_loaded)
  {
    if (SDL_GL_LoadLibrary(nullptr) != 0)
    {
      Error::SetStringFmt(error, "SDL_GL_LoadLibrary() failed: {}", SDL_GetError());
      return false;
    }

    opengl_loaded = true;
  }

  if (wi.IsSurfaceless())
  {
    Error::SetStringView(error, "Surfaceless is not supported with OpenGLContextSDL.");
    return false;
  }
  else if (wi.type != WindowInfo::Type::SDL)
  {
    Error::SetStringView(error, "Incompatible window type.");
    return false;
  }

  SDL_Window* const window = static_cast<SDL_Window*>(wi.window_handle);
  for (const Version& cv : versions_to_try)
  {
    if (CreateVersionContext(cv, window, wi.surface_format, share_context, !share_context, error))
    {
      m_version = cv;
      *surface = window;
      return true;
    }
  }

  Error::SetStringView(error, "Failed to create any contexts.");
  return false;
}

void* OpenGLContextSDL::GetProcAddress(const char* name)
{
  return SDL_GL_GetProcAddress(name);
}

OpenGLContext::SurfaceHandle OpenGLContextSDL::CreateSurface(WindowInfo& wi, Error* error /*= nullptr*/)
{
  if (wi.IsSurfaceless()) [[unlikely]]
  {
    Error::SetStringView(error, "Trying to create a surfaceless surface.");
    return nullptr;
  }
  else if (wi.type != WindowInfo::Type::SDL)
  {
    Error::SetStringView(error, "Incompatible window type.");
    return nullptr;
  }

  return static_cast<SDL_Window*>(wi.window_handle);
}

void OpenGLContextSDL::DestroySurface(SurfaceHandle handle)
{
  // cleaned up on window destruction?
}

void OpenGLContextSDL::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  int drawable_width = wi.surface_width;
  int drawable_height = wi.surface_height;
  SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(handle), &drawable_width, &drawable_height);
  wi.surface_width = static_cast<u16>(drawable_width);
  wi.surface_height = static_cast<u16>(drawable_height);
}

bool OpenGLContextSDL::SwapBuffers()
{
  SDL_GL_SwapWindow(m_current_window);
  return true;
}

bool OpenGLContextSDL::IsCurrent() const
{
  return (m_context && SDL_GL_GetCurrentContext() == m_context);
}

bool OpenGLContextSDL::MakeCurrent(SurfaceHandle surface, Error* error /* = nullptr */)
{
  SDL_Window* const window = static_cast<SDL_Window*>(surface);
  if (m_current_window == window)
    return true;

  if (SDL_GL_MakeCurrent(window, m_context) != 0)
  {
    ERROR_LOG("SDL_GL_MakeCurrent() failed: {}", SDL_GetError());
    return false;
  }

  m_current_window = window;
  return true;
}

bool OpenGLContextSDL::DoneCurrent()
{
  if (SDL_GL_MakeCurrent(nullptr, nullptr) != 0)
    return false;

  m_current_window = nullptr;
  return true;
}

bool OpenGLContextSDL::SupportsNegativeSwapInterval() const
{
  const int current_interval = SDL_GL_GetSwapInterval();
  const bool supported = (SDL_GL_SetSwapInterval(-1) != 0);
  SDL_GL_SetSwapInterval(current_interval);
  return supported;
}

bool OpenGLContextSDL::SetSwapInterval(s32 interval, Error* error)
{
  if (SDL_GL_SetSwapInterval(interval) != 0)
  {
    Error::SetStringFmt(error, "SDL_GL_SetSwapInterval() failed: ", SDL_GetError());
    return false;
  }

  return true;
}

std::unique_ptr<OpenGLContext> OpenGLContextSDL::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                     Error* error)
{
  std::unique_ptr<OpenGLContextSDL> context = std::make_unique<OpenGLContextSDL>();
  if (!context->Initialize(wi, surface, std::span<const Version>(&m_version, 1), true, error))
    context.reset();
  return {};
}

bool OpenGLContextSDL::CreateVersionContext(const Version& version, SDL_Window* window,
                                            GPUTexture::Format surface_format, bool share_context, bool make_current,
                                            Error* error)
{
  SDL_GL_ResetAttributes();

  if (surface_format == GPUTexture::Format::Unknown)
    surface_format = GPUTexture::Format::RGBA8;

  switch (surface_format)
  {
    case GPUTexture::Format::RGBA8:
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
      break;

    case GPUTexture::Format::RGB565:
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
      break;

    default:
      Error::SetStringFmt(error, "Unsupported texture format {}", GPUTexture::GetFormatName(surface_format));
      break;
  }

  if (share_context)
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, true);

  if (version.profile != Profile::NoProfile)
  {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        (version.profile == Profile::ES) ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, version.major_version);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, version.minor_version);
    if (version.profile == Profile::Core)
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  }

  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context)
  {
    Error::SetStringFmt(error, "SDL_GL_CreateContext() failed: {}", SDL_GetError());
    return false;
  }

  if (make_current)
  {
    if (SDL_GL_MakeCurrent(window, context) != 0)
    {
      Error::SetStringFmt(error, "SDL_GL_MakeCurrent() failed: {}", SDL_GetError());
      SDL_GL_DeleteContext(context);
      return false;
    }

    m_current_window = window;
  }

  m_context = context;
  return true;
}
