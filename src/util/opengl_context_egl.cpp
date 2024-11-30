// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_egl.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"

#include <atomic>
#include <cstring>
#include <optional>
#include <vector>

LOG_CHANNEL(GPUDevice);

static DynamicLibrary s_egl_library;
static std::atomic_uint32_t s_egl_refcount = 0;

static bool LoadEGL()
{
  // We're not going to be calling this from multiple threads concurrently.
  // So, not wrapping this in a mutex should be fine.
  if (s_egl_refcount.fetch_add(1, std::memory_order_acq_rel) == 0)
  {
    DebugAssert(!s_egl_library.IsOpen());

    std::string egl_libname = DynamicLibrary::GetVersionedFilename("libEGL");
    INFO_LOG("Loading EGL from {}...", egl_libname);

    Error error;
    if (!s_egl_library.Open(egl_libname.c_str(), &error))
    {
      // Try versioned.
      egl_libname = DynamicLibrary::GetVersionedFilename("libEGL", 1);
      INFO_LOG("Loading EGL from {}...", egl_libname);
      if (!s_egl_library.Open(egl_libname.c_str(), &error))
        ERROR_LOG("Failed to load EGL: {}", error.GetDescription());
    }
  }

  return s_egl_library.IsOpen();
}

static void UnloadEGL()
{
  DebugAssert(s_egl_refcount.load(std::memory_order_acquire) > 0);
  if (s_egl_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
  {
    INFO_LOG("Unloading EGL.");
    s_egl_library.Close();
  }
}

static bool LoadGLADEGL(EGLDisplay display, Error* error)
{
  const int version =
    gladLoadEGL(display, [](const char* name) { return (GLADapiproc)s_egl_library.GetSymbolAddress(name); });
  if (version == 0)
  {
    Error::SetStringView(error, "Loading GLAD EGL functions failed");
    return false;
  }

  DEV_LOG("GLAD EGL Version: {}.{}", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));
  return true;
}

OpenGLContextEGL::OpenGLContextEGL() : OpenGLContext()
{
  LoadEGL();
}

OpenGLContextEGL::~OpenGLContextEGL()
{
  if (m_context != EGL_NO_CONTEXT && eglGetCurrentContext() == m_context)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_pbuffer_surface != EGL_NO_SURFACE)
    eglDestroySurface(m_display, m_pbuffer_surface);

  if (m_context != EGL_NO_CONTEXT)
    eglDestroyContext(m_display, m_context);

  UnloadEGL();
}

std::unique_ptr<OpenGLContext> OpenGLContextEGL::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                        std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextEGL> context = std::make_unique<OpenGLContextEGL>();
  if (!context->Initialize(wi, surface, versions_to_try, error))
    return nullptr;

  return context;
}

bool OpenGLContextEGL::Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try,
                                  Error* error)
{
  if (!LoadGLADEGL(EGL_NO_DISPLAY, error))
    return false;

  m_display = GetPlatformDisplay(wi, error);
  if (m_display == EGL_NO_DISPLAY)
    return false;

  int egl_major, egl_minor;
  if (!eglInitialize(m_display, &egl_major, &egl_minor))
  {
    const int gerror = static_cast<int>(eglGetError());
    Error::SetStringFmt(error, "eglInitialize() failed: {} (0x{:X})", gerror, gerror);
    return false;
  }

  DEV_LOG("eglInitialize() version: {}.{}", egl_major, egl_minor);

  // Re-initialize EGL/GLAD.
  if (!LoadGLADEGL(m_display, error))
    return false;

  if (!GLAD_EGL_KHR_surfaceless_context)
    WARNING_LOG("EGL implementation does not support surfaceless contexts, emulating with pbuffers");

  Error context_error;
  for (const Version& cv : versions_to_try)
  {
    if (CreateContextAndSurface(wi, surface, cv, nullptr, true, &context_error))
    {
      return true;
    }
    else
    {
      WARNING_LOG("Failed to create {}.{} ({}) context: {}", cv.major_version, cv.minor_version,
                  cv.profile == OpenGLContext::Profile::ES ?
                    "ES" :
                    (cv.profile == OpenGLContext::Profile::Core ? "Core" : "None"),
                  context_error.GetDescription());
    }
  }

  Error::SetStringView(error, "Failed to create any context versions");
  return false;
}

EGLDisplay OpenGLContextEGL::GetPlatformDisplay(const WindowInfo& wi, Error* error)
{
  EGLDisplay dpy =
    TryGetPlatformDisplay(wi.display_connection, EGL_PLATFORM_SURFACELESS_MESA, "EGL_MESA_platform_surfaceless");
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(wi.display_connection, error);

  return dpy;
}

EGLSurface OpenGLContextEGL::CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error)
{
  EGLSurface surface = TryCreatePlatformSurface(config, wi.window_handle, error);
  if (surface == EGL_NO_SURFACE)
    surface = CreateFallbackSurface(config, wi.window_handle, error);
  return surface;
}

bool OpenGLContextEGL::SupportsSurfaceless() const
{
  return GLAD_EGL_KHR_surfaceless_context;
}

EGLDisplay OpenGLContextEGL::TryGetPlatformDisplay(void* display, EGLenum platform, const char* platform_ext)
{
  const char* extensions_str = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!extensions_str)
  {
    ERROR_LOG("No extensions supported.");
    return EGL_NO_DISPLAY;
  }

  EGLDisplay dpy = EGL_NO_DISPLAY;
  if (platform_ext && std::strstr(extensions_str, platform_ext))
  {
    DEV_LOG("Using EGL platform {}.", platform_ext);

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display_ext)
    {
      dpy = get_platform_display_ext(platform, display, nullptr);
      m_use_ext_platform_base = (dpy != EGL_NO_DISPLAY);
      if (!m_use_ext_platform_base)
      {
        const EGLint err = eglGetError();
        ERROR_LOG("eglGetPlatformDisplayEXT() failed: {} (0x{:X})", err, err);
      }
    }
    else
    {
      WARNING_LOG("eglGetPlatformDisplayEXT() was not found");
    }
  }
  else
  {
    WARNING_LOG("{} is not supported.", platform_ext);
  }

  return dpy;
}

EGLSurface OpenGLContextEGL::TryCreatePlatformSurface(EGLConfig config, void* window, Error* error)
{
  EGLSurface surface = EGL_NO_SURFACE;
  if (m_use_ext_platform_base)
  {
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface_ext =
      (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    if (create_platform_window_surface_ext)
    {
      surface = create_platform_window_surface_ext(m_display, config, window, nullptr);
      if (surface == EGL_NO_SURFACE)
      {
        const EGLint err = eglGetError();
        Error::SetStringFmt(error, "eglCreatePlatformWindowSurfaceEXT() failed: {} (0x{:X})", err, err);
      }
    }
    else
    {
      ERROR_LOG("eglCreatePlatformWindowSurfaceEXT() not found");
    }
  }

  return surface;
}

EGLDisplay OpenGLContextEGL::GetFallbackDisplay(void* display, Error* error)
{
  WARNING_LOG("Using fallback eglGetDisplay() path.");

  EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)display);
  if (dpy == EGL_NO_DISPLAY)
  {
    const EGLint err = eglGetError();
    Error::SetStringFmt(error, "eglGetDisplay() failed: {} (0x{:X})", err, err);
  }

  return dpy;
}

EGLSurface OpenGLContextEGL::CreateFallbackSurface(EGLConfig config, void* win, Error* error)
{
  WARNING_LOG("Using fallback eglCreateWindowSurface() path.");

  EGLSurface surface = eglCreateWindowSurface(m_display, config, (EGLNativeWindowType)win, nullptr);
  if (surface == EGL_NO_SURFACE)
  {
    const EGLint err = eglGetError();
    Error::SetStringFmt(error, "eglCreateWindowSurface() failed: {} (0x{:X})", err, err);
  }

  return surface;
}

void OpenGLContextEGL::DestroyPlatformSurface(EGLSurface surface)
{
  eglDestroySurface(m_display, surface);
}

void* OpenGLContextEGL::GetProcAddress(const char* name)
{
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

OpenGLContext::SurfaceHandle OpenGLContextEGL::CreateSurface(WindowInfo& wi, Error* error /* = nullptr */)
{
  if (wi.IsSurfaceless()) [[unlikely]]
  {
    Error::SetStringView(error, "Trying to create a surfaceless surface.");
    return nullptr;
  }

  EGLSurface surface = CreatePlatformSurface(m_config, wi, error);
  if (surface == EGL_NO_SURFACE)
    return nullptr;

  UpdateWindowInfoSize(wi, surface);
  return (SurfaceHandle)surface;
}

void OpenGLContextEGL::DestroySurface(SurfaceHandle handle)
{
  // pbuffer surface?
  if (!handle)
    return;

  EGLSurface surface = (EGLSurface)handle;
  if (m_current_surface == surface)
  {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    m_current_surface = EGL_NO_SURFACE;
  }

  DestroyPlatformSurface(surface);
}

void OpenGLContextEGL::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  if (!handle)
    return;

  UpdateWindowInfoSize(wi, (EGLSurface)handle);
}

bool OpenGLContextEGL::SwapBuffers()
{
  return eglSwapBuffers(m_display, m_current_surface);
}

bool OpenGLContextEGL::IsCurrent() const
{
  return m_context && eglGetCurrentContext() == m_context;
}

bool OpenGLContextEGL::MakeCurrent(SurfaceHandle surface, Error* error /* = nullptr */)
{
  EGLSurface esurface = surface ? (EGLSurface)surface : GetSurfacelessSurface();
  if (esurface == m_current_surface)
    return true;

  if (!eglMakeCurrent(m_display, esurface, esurface, m_context)) [[unlikely]]
  {
    Error::SetStringFmt(error, "eglMakeCurrent() failed: 0x{:X}", eglGetError());
    return false;
  }

  m_current_surface = esurface;
  return true;
}

bool OpenGLContextEGL::DoneCurrent()
{
  if (!eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
    return false;

  m_current_surface = EGL_NO_SURFACE;
  return true;
}

bool OpenGLContextEGL::SupportsNegativeSwapInterval() const
{
  return m_supports_negative_swap_interval;
}

bool OpenGLContextEGL::SetSwapInterval(s32 interval, Error* error /* = nullptr */)
{
  if (!eglSwapInterval(m_display, interval))
  {
    Error::SetStringFmt(error, "eglMakeCurrent() failed: 0x{:X}", eglGetError());
    return false;
  }

  return true;
}

std::unique_ptr<OpenGLContext> OpenGLContextEGL::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                     Error* error)
{
  std::unique_ptr<OpenGLContextEGL> context = std::make_unique<OpenGLContextEGL>();
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(wi, surface, m_version, m_context, false, error))
  {
    Error::SetStringView(error, "Failed to create context/surface");
    return nullptr;
  }

  return context;
}

EGLSurface OpenGLContextEGL::GetSurfacelessSurface()
{
  return SupportsSurfaceless() ? EGL_NO_SURFACE : GetPBufferSurface(nullptr);
}

EGLSurface OpenGLContextEGL::GetPBufferSurface(Error* error)
{
  if (m_pbuffer_surface)
    return m_pbuffer_surface;

  EGLint attrib_list[] = {
    EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
  };

  m_pbuffer_surface = eglCreatePbufferSurface(m_display, m_config, attrib_list);
  if (!m_pbuffer_surface) [[unlikely]]
  {
    if (error)
      error->SetStringFmt("eglCreatePbufferSurface() failed: {}", eglGetError());
    else
      ERROR_LOG("eglCreatePbufferSurface() failed: {}", eglGetError());

    return nullptr;
  }

  DEV_LOG("Created pbuffer surface");
  return m_pbuffer_surface;
}

bool OpenGLContextEGL::CheckConfigSurfaceFormat(EGLConfig config, GPUTexture::Format format)
{
  int red_size, green_size, blue_size, alpha_size;
  if (!eglGetConfigAttrib(m_display, config, EGL_RED_SIZE, &red_size) ||
      !eglGetConfigAttrib(m_display, config, EGL_GREEN_SIZE, &green_size) ||
      !eglGetConfigAttrib(m_display, config, EGL_BLUE_SIZE, &blue_size) ||
      !eglGetConfigAttrib(m_display, config, EGL_ALPHA_SIZE, &alpha_size))
  {
    return false;
  }

  switch (format)
  {
    case GPUTexture::Format::RGBA8:
      return (red_size == 8 && green_size == 8 && blue_size == 8 && alpha_size == 8);

    case GPUTexture::Format::RGB565:
      return (red_size == 5 && green_size == 6 && blue_size == 5);

    case GPUTexture::Format::RGB5A1:
      return (red_size == 5 && green_size == 5 && blue_size == 5 && alpha_size == 1);

    case GPUTexture::Format::Unknown:
      return true;

    default:
      return false;
  }
}

void OpenGLContextEGL::UpdateWindowInfoSize(WindowInfo& wi, EGLSurface surface) const
{
  // Some implementations may require the size to be queried at runtime.
  EGLint surface_width, surface_height;
  if (eglQuerySurface(m_display, surface, EGL_WIDTH, &surface_width) &&
      eglQuerySurface(m_display, surface, EGL_HEIGHT, &surface_height))
  {
    wi.surface_width = static_cast<u16>(surface_width);
    wi.surface_height = static_cast<u16>(surface_height);
  }
  else
  {
    ERROR_LOG("eglQuerySurface() failed: 0x{:X}", eglGetError());
  }

  int red_size = 0, green_size = 0, blue_size = 0, alpha_size = 0;
  eglGetConfigAttrib(m_display, m_config, EGL_RED_SIZE, &red_size);
  eglGetConfigAttrib(m_display, m_config, EGL_GREEN_SIZE, &green_size);
  eglGetConfigAttrib(m_display, m_config, EGL_BLUE_SIZE, &blue_size);
  eglGetConfigAttrib(m_display, m_config, EGL_ALPHA_SIZE, &alpha_size);

  if (red_size == 5 && green_size == 6 && blue_size == 5)
  {
    wi.surface_format = GPUTexture::Format::RGB565;
  }
  else if (red_size == 5 && green_size == 5 && blue_size == 5 && alpha_size == 1)
  {
    wi.surface_format = GPUTexture::Format::RGB5A1;
  }
  else if (red_size == 8 && green_size == 8 && blue_size == 8 && alpha_size == 8)
  {
    wi.surface_format = GPUTexture::Format::RGBA8;
  }
  else
  {
    ERROR_LOG("Unknown surface format: R={}, G={}, B={}, A={}", red_size, green_size, blue_size, alpha_size);
    wi.surface_format = GPUTexture::Format::RGBA8;
  }
}

bool OpenGLContextEGL::CreateContext(bool surfaceless, GPUTexture::Format surface_format, const Version& version,
                                     EGLContext share_context, Error* error)
{
  DEV_LOG("Trying version {}.{} ({})", version.major_version, version.minor_version,
          version.profile == OpenGLContext::Profile::ES ?
            "ES" :
            (version.profile == OpenGLContext::Profile::Core ? "Core" : "None"));
  int surface_attribs[16] = {
    EGL_RENDERABLE_TYPE,
    (version.profile == Profile::ES) ?
      ((version.major_version >= 3) ? EGL_OPENGL_ES3_BIT :
                                      ((version.major_version == 2) ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT)) :
      EGL_OPENGL_BIT,
    EGL_SURFACE_TYPE,
    surfaceless ? 0 : EGL_WINDOW_BIT,
  };
  int nsurface_attribs = 4;

  if (surface_format == GPUTexture::Format::Unknown)
    surface_format = GPUTexture::Format::RGBA8;

  switch (surface_format)
  {
    case GPUTexture::Format::RGBA8:
      surface_attribs[nsurface_attribs++] = EGL_RED_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_GREEN_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_BLUE_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_ALPHA_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      break;

    case GPUTexture::Format::RGB565:
      surface_attribs[nsurface_attribs++] = EGL_RED_SIZE;
      surface_attribs[nsurface_attribs++] = 5;
      surface_attribs[nsurface_attribs++] = EGL_GREEN_SIZE;
      surface_attribs[nsurface_attribs++] = 6;
      surface_attribs[nsurface_attribs++] = EGL_BLUE_SIZE;
      surface_attribs[nsurface_attribs++] = 5;
      break;

    default:
      Error::SetStringFmt(error, "Unsupported texture format {}", GPUTexture::GetFormatName(surface_format));
      break;
  }

  surface_attribs[nsurface_attribs++] = EGL_NONE;
  surface_attribs[nsurface_attribs++] = 0;

  EGLint num_configs;
  if (!eglChooseConfig(m_display, surface_attribs, nullptr, 0, &num_configs) || num_configs == 0)
  {
    Error::SetStringFmt(error, "eglChooseConfig() failed: 0x{:x}", static_cast<unsigned>(eglGetError()));
    return false;
  }

  std::vector<EGLConfig> configs(static_cast<u32>(num_configs));
  if (!eglChooseConfig(m_display, surface_attribs, configs.data(), num_configs, &num_configs))
  {
    Error::SetStringFmt(error, "eglChooseConfig() failed: 0x{:x}", static_cast<unsigned>(eglGetError()));
    return false;
  }
  configs.resize(static_cast<u32>(num_configs));

  std::optional<EGLConfig> config;
  for (EGLConfig check_config : configs)
  {
    if (CheckConfigSurfaceFormat(check_config, surface_format))
    {
      config = check_config;
      break;
    }
  }

  if (!config.has_value())
  {
    WARNING_LOG("No EGL configs matched exactly, using first.");
    config = configs.front();
  }

  int attribs[8];
  int nattribs = 0;
  if (version.profile != Profile::NoProfile)
  {
    attribs[nattribs++] = EGL_CONTEXT_MAJOR_VERSION;
    attribs[nattribs++] = version.major_version;
    attribs[nattribs++] = EGL_CONTEXT_MINOR_VERSION;
    attribs[nattribs++] = version.minor_version;
  }
  attribs[nattribs++] = EGL_NONE;
  attribs[nattribs++] = 0;

  if (!eglBindAPI((version.profile == Profile::ES) ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
  {
    Error::SetStringFmt(error, "eglBindAPI({}) failed",
                        (version.profile == Profile::ES) ? "EGL_OPENGL_ES_API" : "EGL_OPENGL_API");
    return false;
  }

  m_context = eglCreateContext(m_display, config.value(), share_context, attribs);
  if (!m_context)
  {
    Error::SetStringFmt(error, "eglCreateContext() failed: 0x{:x}", static_cast<unsigned>(eglGetError()));
    return false;
  }

  INFO_LOG("Got version {}.{} ({})", version.major_version, version.minor_version,
           version.profile == OpenGLContext::Profile::ES ?
             "ES" :
             (version.profile == OpenGLContext::Profile::Core ? "Core" : "None"));

  EGLint min_swap_interval, max_swap_interval;
  m_supports_negative_swap_interval = false;
  if (eglGetConfigAttrib(m_display, config.value(), EGL_MIN_SWAP_INTERVAL, &min_swap_interval) &&
      eglGetConfigAttrib(m_display, config.value(), EGL_MAX_SWAP_INTERVAL, &max_swap_interval))
  {
    VERBOSE_LOG("EGL_MIN_SWAP_INTERVAL = {}", min_swap_interval);
    VERBOSE_LOG("EGL_MAX_SWAP_INTERVAL = {}", max_swap_interval);
    m_supports_negative_swap_interval = (min_swap_interval <= -1);
  }

  INFO_LOG("Negative swap interval/tear-control is {}supported", m_supports_negative_swap_interval ? "" : "NOT ");

  m_config = config.value();
  m_version = version;
  return true;
}

bool OpenGLContextEGL::CreateContextAndSurface(WindowInfo& wi, SurfaceHandle* surface, const Version& version,
                                               EGLContext share_context, bool make_current, Error* error)
{
  if (!CreateContext(wi.IsSurfaceless(), wi.surface_format, version, share_context, error))
    return false;

  // create actual surface, need to handle surfaceless here
  EGLSurface esurface;
  if (wi.IsSurfaceless())
  {
    if (!SupportsSurfaceless())
    {
      esurface = GetPBufferSurface(error);
      if (esurface == EGL_NO_SURFACE)
      {
        ERROR_LOG("Failed to create pbuffer surface for context");
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_SURFACE;
        return false;
      }
    }
    else
    {
      esurface = EGL_NO_SURFACE;
    }

    *surface = nullptr;
  }
  else
  {
    esurface = CreatePlatformSurface(m_config, wi, error);
    if (esurface == EGL_NO_SURFACE)
    {
      ERROR_LOG("Failed to create surface for context");
      eglDestroyContext(m_display, m_context);
      m_context = EGL_NO_SURFACE;
      return false;
    }

    UpdateWindowInfoSize(wi, esurface);
    *surface = esurface;
  }

  if (make_current)
  {
    if (!eglMakeCurrent(m_display, esurface, esurface, m_context))
    {
      Error::SetStringFmt(error, "eglMakeCurrent() failed: 0x{:X}", eglGetError());
      if (esurface != EGL_NO_SURFACE && esurface != m_pbuffer_surface)
        DestroyPlatformSurface(esurface);
      eglDestroyContext(m_display, m_context);
      m_context = EGL_NO_CONTEXT;
      return false;
    }

    m_current_surface = esurface;
  }

  return true;
}
