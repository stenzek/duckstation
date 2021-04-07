#include "context_egl.h"
#include "../assert.h"
#include "../log.h"
#include <optional>
#include <vector>
Log_SetChannel(GL::ContextEGL);

namespace GL {
ContextEGL::ContextEGL(const WindowInfo& wi) : Context(wi) {}

ContextEGL::~ContextEGL()
{
  DestroySurface();
  DestroyContext();
}

std::unique_ptr<Context> ContextEGL::Create(const WindowInfo& wi, const Version* versions_to_try,
                                            size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGL> context = std::make_unique<ContextEGL>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

bool ContextEGL::Initialize(const Version* versions_to_try, size_t num_versions_to_try)
{
  if (!gladLoadEGL())
  {
    Log_ErrorPrintf("Loading GLAD EGL functions failed");
    return false;
  }

  if (!SetDisplay())
    return false;

  int egl_major, egl_minor;
  if (!eglInitialize(m_display, &egl_major, &egl_minor))
  {
    Log_ErrorPrintf("eglInitialize() failed: %d", eglGetError());
    return false;
  }
  Log_InfoPrintf("EGL Version: %d.%d", egl_major, egl_minor);

  const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
  if (extensions)
  {
    Log_InfoPrintf("EGL Extensions: %s", extensions);
    m_supports_surfaceless = std::strstr(extensions, "EGL_KHR_surfaceless_context") != nullptr;
  }
  if (!m_supports_surfaceless)
    Log_WarningPrint("EGL implementation does not support surfaceless contexts, emulating with pbuffers");

  for (size_t i = 0; i < num_versions_to_try; i++)
  {
    if (CreateContextAndSurface(versions_to_try[i], nullptr, true))
      return true;
  }

  return false;
}

bool ContextEGL::SetDisplay()
{
  m_display = eglGetDisplay(static_cast<EGLNativeDisplayType>(m_wi.display_connection));
  if (!m_display)
  {
    Log_ErrorPrintf("eglGetDisplay() failed: %d", eglGetError());
    return false;
  }

  return true;
}

void* ContextEGL::GetProcAddress(const char* name)
{
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

bool ContextEGL::ChangeSurface(const WindowInfo& new_wi)
{
  const bool was_current = (eglGetCurrentContext() == m_context);
  if (was_current)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_surface != EGL_NO_SURFACE)
  {
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
  }

  m_wi = new_wi;
  if (!CreateSurface())
    return false;

  if (was_current && !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
  {
    Log_ErrorPrintf("Failed to make context current again after surface change");
    return false;
  }

  return true;
}

void ContextEGL::ResizeSurface(u32 new_surface_width /*= 0*/, u32 new_surface_height /*= 0*/)
{
  if (new_surface_width == 0 && new_surface_height == 0)
  {
    EGLint surface_width, surface_height;
    if (eglQuerySurface(m_display, m_surface, EGL_WIDTH, &surface_width) &&
        eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &surface_height))
    {
      m_wi.surface_width = static_cast<u32>(surface_width);
      m_wi.surface_height = static_cast<u32>(surface_height);
      return;
    }
    else
    {
      Log_ErrorPrintf("eglQuerySurface() failed: %d", eglGetError());
    }
  }

  m_wi.surface_width = new_surface_width;
  m_wi.surface_height = new_surface_height;
}

bool ContextEGL::SwapBuffers()
{
  return eglSwapBuffers(m_display, m_surface);
}

bool ContextEGL::MakeCurrent()
{
  if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
  {
    Log_ErrorPrintf("eglMakeCurrent() failed: %d", eglGetError());
    return false;
  }

  return true;
}

bool ContextEGL::DoneCurrent()
{
  return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool ContextEGL::SetSwapInterval(s32 interval)
{
  return eglSwapInterval(m_display, interval);
}

std::unique_ptr<Context> ContextEGL::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGL> context = std::make_unique<ContextEGL>(wi);
  context->m_display = m_display;
  context->m_supports_surfaceless = m_supports_surfaceless;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLNativeWindowType ContextEGL::GetNativeWindow(EGLConfig config)
{
  return {};
}

bool ContextEGL::CreateSurface()
{
  if (m_wi.type == WindowInfo::Type::Surfaceless)
  {
    if (m_supports_surfaceless)
      return true;
    else
      return CreatePBufferSurface();
  }

  EGLNativeWindowType native_window = GetNativeWindow(m_config);
  m_surface = eglCreateWindowSurface(m_display, m_config, native_window, nullptr);
  if (!m_surface)
  {
    Log_ErrorPrintf("eglCreateWindowSurface() failed: %d", eglGetError());
    return false;
  }

  // Some implementations may require the size to be queried at runtime.
  EGLint surface_width, surface_height;
  if (eglQuerySurface(m_display, m_surface, EGL_WIDTH, &surface_width) &&
      eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &surface_height))
  {
    m_wi.surface_width = static_cast<u32>(surface_width);
    m_wi.surface_height = static_cast<u32>(surface_height);
  }
  else
  {
    Log_ErrorPrintf("eglQuerySurface() failed: %d", eglGetError());
  }

  return true;
}

bool ContextEGL::CreatePBufferSurface()
{
  const u32 width = std::max<u32>(m_wi.surface_width, 1);
  const u32 height = std::max<u32>(m_wi.surface_height, 1);

  // TODO: Format
  EGLint attrib_list[] = {
    EGL_WIDTH, static_cast<EGLint>(width), EGL_HEIGHT, static_cast<EGLint>(height), EGL_NONE,
  };

  m_surface = eglCreatePbufferSurface(m_display, m_config, attrib_list);
  if (!m_surface)
  {
    Log_ErrorPrintf("eglCreatePbufferSurface() failed: %d", eglGetError());
    return false;
  }

  Log_DevPrintf("Created %ux%u pbuffer surface", width, height);
  return true;
}

bool ContextEGL::CheckConfigSurfaceFormat(EGLConfig config, WindowInfo::SurfaceFormat format) const
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
    case WindowInfo::SurfaceFormat::Auto:
      return true;

    case WindowInfo::SurfaceFormat::RGB8:
      return (red_size == 8 && green_size == 8 && blue_size == 8);

    case WindowInfo::SurfaceFormat::RGBA8:
      return (red_size == 8 && green_size == 8 && blue_size == 8 && alpha_size == 8);

    case WindowInfo::SurfaceFormat::RGB565:
      return (red_size == 5 && green_size == 6 && blue_size == 5);

    default:
      return false;
  }
}

void ContextEGL::DestroyContext()
{
  if (eglGetCurrentContext() == m_context)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_context != EGL_NO_CONTEXT)
  {
    eglDestroyContext(m_display, m_context);
    m_context = EGL_NO_CONTEXT;
  }
}

void ContextEGL::DestroySurface()
{
  if (eglGetCurrentSurface(EGL_DRAW) == m_surface)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_surface != EGL_NO_SURFACE)
  {
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
  }
}

bool ContextEGL::CreateContext(const Version& version, EGLContext share_context)
{
  Log_DevPrintf(
    "Trying version %u.%u (%s)", version.major_version, version.minor_version,
    version.profile == Context::Profile::ES ? "ES" : (version.profile == Context::Profile::Core ? "Core" : "None"));
  int surface_attribs[16] = {
    EGL_RENDERABLE_TYPE,
    (version.profile == Profile::ES) ?
      ((version.major_version >= 3) ? EGL_OPENGL_ES3_BIT :
                                      ((version.major_version == 2) ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT)) :
      EGL_OPENGL_BIT,
    EGL_SURFACE_TYPE,
    (m_wi.type != WindowInfo::Type::Surfaceless) ? EGL_WINDOW_BIT : 0,
  };
  int nsurface_attribs = 4;

  switch (m_wi.surface_format)
  {
    case WindowInfo::SurfaceFormat::RGB8:
      surface_attribs[nsurface_attribs++] = EGL_RED_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_GREEN_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_BLUE_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      break;

    case WindowInfo::SurfaceFormat::RGBA8:
      surface_attribs[nsurface_attribs++] = EGL_RED_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_GREEN_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_BLUE_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      surface_attribs[nsurface_attribs++] = EGL_ALPHA_SIZE;
      surface_attribs[nsurface_attribs++] = 8;
      break;

    case WindowInfo::SurfaceFormat::RGB565:
      surface_attribs[nsurface_attribs++] = EGL_RED_SIZE;
      surface_attribs[nsurface_attribs++] = 5;
      surface_attribs[nsurface_attribs++] = EGL_GREEN_SIZE;
      surface_attribs[nsurface_attribs++] = 6;
      surface_attribs[nsurface_attribs++] = EGL_BLUE_SIZE;
      surface_attribs[nsurface_attribs++] = 5;
      break;

    case WindowInfo::SurfaceFormat::Auto:
      break;

    default:
      UnreachableCode();
      break;
  }

  surface_attribs[nsurface_attribs++] = EGL_NONE;
  surface_attribs[nsurface_attribs++] = 0;

  EGLint num_configs;
  if (!eglChooseConfig(m_display, surface_attribs, nullptr, 0, &num_configs) || num_configs == 0)
  {
    Log_ErrorPrintf("eglChooseConfig() failed: %d", eglGetError());
    return false;
  }

  std::vector<EGLConfig> configs(static_cast<u32>(num_configs));
  if (!eglChooseConfig(m_display, surface_attribs, configs.data(), num_configs, &num_configs))
  {
    Log_ErrorPrintf("eglChooseConfig() failed: %d", eglGetError());
    return false;
  }
  configs.resize(static_cast<u32>(num_configs));

  std::optional<EGLConfig> config;
  for (EGLConfig check_config : configs)
  {
    if (CheckConfigSurfaceFormat(check_config, m_wi.surface_format))
    {
      config = check_config;
      break;
    }
  }

  if (!config.has_value())
  {
    Log_WarningPrintf("No EGL configs matched exactly, using first.");
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
    Log_ErrorPrintf("eglBindAPI(%s) failed", (version.profile == Profile::ES) ? "EGL_OPENGL_ES_API" : "EGL_OPENGL_API");
    return false;
  }

  m_context = eglCreateContext(m_display, config.value(), share_context, attribs);
  if (!m_context)
  {
    Log_ErrorPrintf("eglCreateContext() failed: %d", eglGetError());
    return false;
  }

  Log_InfoPrintf(
    "Got version %u.%u (%s)", version.major_version, version.minor_version,
    version.profile == Context::Profile::ES ? "ES" : (version.profile == Context::Profile::Core ? "Core" : "None"));

  m_config = config.value();
  m_version = version;
  return true;
}

bool ContextEGL::CreateContextAndSurface(const Version& version, EGLContext share_context, bool make_current)
{
  if (!CreateContext(version, share_context))
    return false;

  if (!CreateSurface())
  {
    Log_ErrorPrintf("Failed to create surface for context");
    eglDestroyContext(m_display, m_context);
    m_context = EGL_NO_CONTEXT;
    return false;
  }

  if (make_current && !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
  {
    Log_ErrorPrintf("eglMakeCurrent() failed: %d", eglGetError());
    if (m_surface != EGL_NO_SURFACE)
    {
      eglDestroySurface(m_display, m_surface);
      m_surface = EGL_NO_SURFACE;
    }
    eglDestroyContext(m_display, m_context);
    m_context = EGL_NO_CONTEXT;
    return false;
  }

  return true;
}
} // namespace GL
