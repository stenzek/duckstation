// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "context.h"
#include "../opengl_loader.h"

#include "common/error.h"
#include "common/log.h"

#include <cstdio>
#include <cstdlib>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif

#if defined(_WIN32) && !defined(_M_ARM64)
#include "context_wgl.h"
#elif defined(__APPLE__)
#include "context_agl.h"
#elif defined(__ANDROID__)
#include "context_egl_android.h"
#else
#ifdef ENABLE_EGL
#ifdef ENABLE_WAYLAND
#include "context_egl_wayland.h"
#endif
#ifdef ENABLE_X11
#include "context_egl_x11.h"
#endif
#endif
#endif

Log_SetChannel(GL::Context);

namespace GL {

static bool ShouldPreferESContext()
{
#if defined(__ANDROID__)
  return true;
#elif !defined(_MSC_VER)
  const char* value = std::getenv("PREFER_GLES_CONTEXT");
  return (value && std::strcmp(value, "1") == 0);
#else
  char buffer[2] = {};
  size_t buffer_size = sizeof(buffer);
  getenv_s(&buffer_size, buffer, "PREFER_GLES_CONTEXT");
  return (std::strcmp(buffer, "1") == 0);
#endif
}

static void DisableBrokenExtensions(const char* gl_vendor, const char* gl_renderer, const char* gl_version)
{
  if (std::strstr(gl_vendor, "ARM"))
  {
    // GL_{EXT,OES}_copy_image seem to be implemented on the CPU in the Mali drivers...
    // Older drivers don't implement timer queries correctly either.
    int gl_major_version, gl_minor_version, unused_version, major_version, patch_version;
    if ((std::sscanf(gl_version, "OpenGL ES %d.%d v%d.r%dp%d", &gl_major_version, &gl_minor_version, &unused_version,
                     &major_version, &patch_version) == 5 &&
         gl_major_version >= 3 && gl_minor_version >= 2 && major_version >= 32) ||
        (std::sscanf(gl_version, "OpenGL ES %d.%d v%d.g%dp%d", &gl_major_version, &gl_minor_version, &unused_version,
                     &major_version, &patch_version) == 5 &&
         gl_major_version >= 3 && gl_minor_version >= 2 && major_version > 0))
    {
      // r32p0 and beyond seem okay.
      // Log_VerbosePrintf("Keeping copy_image for driver version '%s'", gl_version);

      // Framebuffer blits still end up faster.
      Log_VerbosePrintf("Newer Mali driver detected, disabling GL_{EXT,OES}_copy_image.");
      GLAD_GL_EXT_copy_image = 0;
      GLAD_GL_OES_copy_image = 0;
    }
    else
    {
      Log_VerbosePrintf("Older Mali driver detected, disabling GL_{EXT,OES}_copy_image, disjoint_timer_query.");
      GLAD_GL_EXT_copy_image = 0;
      GLAD_GL_OES_copy_image = 0;
      GLAD_GL_EXT_disjoint_timer_query = 0;
    }
  }
  else if (std::strstr(gl_vendor, "Qualcomm") && std::strstr(gl_renderer, "Adreno"))
  {
    // Framebuffer fetch appears to be broken in drivers ?? >= 464 < 502.
    int gl_major_version = 0, gl_minor_version = 0, major_version = 0;
    if ((std::sscanf(gl_version, "OpenGL ES %d.%d V@%d", &gl_major_version, &gl_minor_version, &major_version) == 3 &&
         gl_major_version >= 3 && gl_minor_version >= 2 && major_version < 502))
    {
      Log_VerboseFmt("Disabling GL_EXT_shader_framebuffer_fetch on Adreno version {}", major_version);
      GLAD_GL_EXT_shader_framebuffer_fetch = 0;
      GLAD_GL_ARM_shader_framebuffer_fetch = 0;
    }
    else
    {
      Log_VerboseFmt("Keeping GL_EXT_shader_framebuffer_fetch on Adreno version {}", major_version);
    }
  }

  // If we're missing GLES 3.2, but have OES_draw_elements_base_vertex, redirect the function pointers.
  if (!glad_glDrawElementsBaseVertex && GLAD_GL_OES_draw_elements_base_vertex && !GLAD_GL_ES_VERSION_3_2)
  {
    glad_glDrawElementsBaseVertex = glad_glDrawElementsBaseVertexOES;
    glad_glDrawRangeElementsBaseVertex = glad_glDrawRangeElementsBaseVertexOES;
    glad_glDrawElementsInstancedBaseVertex = glad_glDrawElementsInstancedBaseVertexOES;
  }
}

Context::Context(const WindowInfo& wi) : m_wi(wi)
{
}

Context::~Context() = default;

std::vector<Context::FullscreenModeInfo> Context::EnumerateFullscreenModes()
{
  return {};
}

std::unique_ptr<GL::Context> Context::Create(const WindowInfo& wi, Error* error)
{
  static constexpr std::array<Version, 14> vlist = {{{Profile::Core, 4, 6},
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
                                                     {Profile::ES, 3, 0}}};

  std::span<const Version> versions_to_try = vlist;
  if (ShouldPreferESContext())
  {
    // move ES versions to the front
    Version* new_versions_to_try = static_cast<Version*>(alloca(sizeof(Version) * versions_to_try.size()));
    size_t count = 0;
    for (const Version& cv : versions_to_try)
    {
      if (cv.profile == Profile::ES)
        new_versions_to_try[count++] = cv;
    }
    for (const Version& cv : versions_to_try)
    {
      if (cv.profile != Profile::ES)
        new_versions_to_try[count++] = cv;
    }
    versions_to_try = std::span<const Version>(new_versions_to_try, versions_to_try.size());
  }

  std::unique_ptr<Context> context;
#if defined(_WIN32) && !defined(_M_ARM64)
  context = ContextWGL::Create(wi, versions_to_try, error);
#elif defined(__APPLE__)
  context = ContextAGL::Create(wi, versions_to_try);
#elif defined(__ANDROID__)
  context = ContextEGLAndroid::Create(wi, versions_to_try, error);
#else
#if defined(ENABLE_X11)
  if (wi.type == WindowInfo::Type::X11)
    context = ContextEGLX11::Create(wi, versions_to_try, error);
#endif
#if defined(ENABLE_WAYLAND)
  if (wi.type == WindowInfo::Type::Wayland)
    context = ContextEGLWayland::Create(wi, versions_to_try, error);
#endif
  if (wi.type == WindowInfo::Type::Surfaceless)
    context = ContextEGL::Create(wi, versions_to_try, error);
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
      Error::SetStringView(error, "Failed to load GL functions for GLAD");
      return nullptr;
    }
  }
  else
  {
    if (!gladLoadGLES2Loader([](const char* name) { return context_being_created->GetProcAddress(name); }))
    {
      Error::SetStringView(error, "Failed to load GLES functions for GLAD");
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

  DisableBrokenExtensions(gl_vendor, gl_renderer, gl_version);

  return context;
}

} // namespace GL
