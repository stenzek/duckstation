#include "context.h"
#include "../log.h"
#include "loader.h"
#include <cstdlib>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif
Log_SetChannel(GL::Context);

#if defined(_WIN32) && !defined(_M_ARM64)
#include "context_wgl.h"
#elif defined(__APPLE__) && !defined(LIBERTRO)
#include "context_agl.h"
#endif

#ifdef USE_EGL
#if defined(USE_WAYLAND) || defined(USE_GBM) || defined(USE_FBDEV) || defined(USE_X11)
#if defined(USE_WAYLAND)
#include "context_egl_wayland.h"
#endif
#if defined(USE_GBM)
#include "context_egl_gbm.h"
#endif
#if defined(USE_FBDEV)
#include "context_egl_fbdev.h"
#endif
#if defined(USE_X11)
#include "context_egl_x11.h"
#endif
#elif defined(ANDROID)
#include "context_egl_android.h"
#else
#error Unknown EGL platform
#endif
#endif

#ifdef USE_GLX
#include "context_glx.h"
#endif

namespace GL {

static bool ShouldPreferESContext()
{
#ifndef _MSC_VER
  const char* value = std::getenv("PREFER_GLES_CONTEXT");
  return (value && std::strcmp(value, "1") == 0);
#else
  char buffer[2] = {};
  size_t buffer_size = sizeof(buffer);
  getenv_s(&buffer_size, buffer, "PREFER_GLES_CONTEXT");
  return (std::strcmp(buffer, "1") == 0);
#endif
}

static void DisableBrokenExtensions(const char* gl_vendor, const char* gl_renderer)
{
  if (std::strstr(gl_vendor, "ARM"))
  {
    // GL_{EXT,OES}_copy_image seem to be implemented on the CPU in the Mali drivers...
    Log_VerbosePrintf("Mali driver detected, disabling GL_{EXT,OES}_copy_image");
    GLAD_GL_EXT_copy_image = 0;
    GLAD_GL_OES_copy_image = 0;
  }
}

Context::Context(const WindowInfo& wi) : m_wi(wi) {}

Context::~Context() = default;

std::vector<Context::FullscreenModeInfo> Context::EnumerateFullscreenModes()
{
  return {};
}

std::unique_ptr<GL::Context> Context::Create(const WindowInfo& wi, const Version* versions_to_try,
                                             size_t num_versions_to_try)
{
  if (ShouldPreferESContext())
  {
    // move ES versions to the front
    Version* new_versions_to_try = static_cast<Version*>(alloca(sizeof(Version) * num_versions_to_try));
    size_t count = 0;
    for (size_t i = 0; i < num_versions_to_try; i++)
    {
      if (versions_to_try[i].profile == Profile::ES)
        new_versions_to_try[count++] = versions_to_try[i];
    }
    for (size_t i = 0; i < num_versions_to_try; i++)
    {
      if (versions_to_try[i].profile != Profile::ES)
        new_versions_to_try[count++] = versions_to_try[i];
    }
    versions_to_try = new_versions_to_try;
  }

  std::unique_ptr<Context> context;
#if defined(_WIN32) && !defined(_M_ARM64)
  context = ContextWGL::Create(wi, versions_to_try, num_versions_to_try);
#elif defined(__APPLE__)
  context = ContextAGL::Create(wi, versions_to_try, num_versions_to_try);
#elif defined(ANDROID)
#ifdef USE_EGL
  context = ContextEGLAndroid::Create(wi, versions_to_try, num_versions_to_try);
#endif
#endif

#if defined(USE_X11)
  if (wi.type == WindowInfo::Type::X11)
  {
#ifdef USE_EGL
    const char* use_egl_x11 = std::getenv("USE_EGL_X11");
    if (use_egl_x11 && std::strcmp(use_egl_x11, "1") == 0)
      context = ContextEGLX11::Create(wi, versions_to_try, num_versions_to_try);
    else
      context = ContextGLX::Create(wi, versions_to_try, num_versions_to_try);
#else
    context = ContextGLX::Create(wi, versions_to_try, num_versions_to_try);
#endif
  }
#endif

#if defined(USE_WAYLAND)
  if (wi.type == WindowInfo::Type::Wayland)
    context = ContextEGLWayland::Create(wi, versions_to_try, num_versions_to_try);
#endif

#if defined(USE_GBM)
  if (wi.type == WindowInfo::Type::Display)
    context = ContextEGLGBM::Create(wi, versions_to_try, num_versions_to_try);
#endif

#if defined(USE_FBDEV)
  if (wi.type == WindowInfo::Type::Display)
    context = ContextEGLFBDev::Create(wi, versions_to_try, num_versions_to_try);
#endif

  if (!context)
    return nullptr;

  Log_InfoPrintf("Created a %s context", context->IsGLES() ? "OpenGL ES" : "OpenGL");

  // TODO: Not thread-safe.
  static Context* context_being_created;
  context_being_created = context.get();

  // load up glad
  if (!context->IsGLES())
  {
    if (!gladLoadGLLoader([](const char* name) { return context_being_created->GetProcAddress(name); }))
    {
      Log_ErrorPrintf("Failed to load GL functions for GLAD");
      return nullptr;
    }
  }
  else
  {
    if (!gladLoadGLES2Loader([](const char* name) { return context_being_created->GetProcAddress(name); }))
    {
      Log_ErrorPrintf("Failed to load GLES functions for GLAD");
      return nullptr;
    }
  }

  const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const char* gl_shading_language_version = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  Log_InfoPrintf("GL_VENDOR: %s", gl_vendor);
  Log_InfoPrintf("GL_RENDERER: %s", gl_renderer);
  Log_InfoPrintf("GL_VERSION: %s", gl_version);
  Log_InfoPrintf("GL_SHADING_LANGUAGE_VERSION: %s", gl_shading_language_version);

  DisableBrokenExtensions(gl_vendor, gl_renderer);

  return context;
}

const std::array<Context::Version, 11>& Context::GetAllDesktopVersionsList()
{
  static constexpr std::array<Version, 11> vlist = {{{Profile::Core, 4, 6},
                                                     {Profile::Core, 4, 5},
                                                     {Profile::Core, 4, 4},
                                                     {Profile::Core, 4, 3},
                                                     {Profile::Core, 4, 2},
                                                     {Profile::Core, 4, 1},
                                                     {Profile::Core, 4, 0},
                                                     {Profile::Core, 3, 3},
                                                     {Profile::Core, 3, 2},
                                                     {Profile::Core, 3, 1},
                                                     {Profile::Core, 3, 0}}};
  return vlist;
}

const std::array<Context::Version, 12>& Context::GetAllDesktopVersionsListWithFallback()
{
  static constexpr std::array<Version, 12> vlist = {{{Profile::Core, 4, 6},
                                                     {Profile::Core, 4, 5},
                                                     {Profile::Core, 4, 4},
                                                     {Profile::Core, 4, 3},
                                                     {Profile::Core, 4, 2},
                                                     {Profile::Core, 4, 1},
                                                     {Profile::Core, 4, 0},
                                                     {Profile::Core, 3, 3},
                                                     {Profile::Core, 3, 2},
                                                     {Profile::Core, 3, 1},
                                                     {Profile::Core, 3, 0},
                                                     {Profile::NoProfile, 0, 0}}};
  return vlist;
}

const std::array<Context::Version, 4>& Context::GetAllESVersionsList()
{
  static constexpr std::array<Version, 4> vlist = {
    {{Profile::ES, 3, 2}, {Profile::ES, 3, 1}, {Profile::ES, 3, 0}, {Profile::ES, 2, 0}}};
  return vlist;
}

const std::array<Context::Version, 16>& Context::GetAllVersionsList()
{
  static constexpr std::array<Version, 16> vlist = {{{Profile::Core, 4, 6},
                                                     {Profile::Core, 4, 5},
                                                     {Profile::Core, 4, 4},
                                                     {Profile::Core, 4, 3},
                                                     {Profile::Core, 4, 2},
                                                     {Profile::Core, 4, 1},
                                                     {Profile::Core, 4, 0},
                                                     {Profile::Core, 3, 3},
                                                     {Profile::Core, 3, 2},
                                                     {Profile::Core, 3, 1},
                                                     {Profile::Core, 3, 0},
                                                     {Profile::ES, 3, 2},
                                                     {Profile::ES, 3, 1},
                                                     {Profile::ES, 3, 0},
                                                     {Profile::ES, 2, 0},
                                                     {Profile::NoProfile, 0, 0}}};
  return vlist;
}

} // namespace GL
