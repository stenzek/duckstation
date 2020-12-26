#include "libretro_opengl_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "core/gpu.h"
#include "libretro.h"
#include "libretro_host_interface.h"
#include <array>
#include <tuple>
Log_SetChannel(LibretroOpenGLHostDisplay);

LibretroOpenGLHostDisplay::LibretroOpenGLHostDisplay() = default;

LibretroOpenGLHostDisplay::~LibretroOpenGLHostDisplay() = default;

HostDisplay::RenderAPI LibretroOpenGLHostDisplay::GetRenderAPI() const
{
  return m_is_gles ? HostDisplay::RenderAPI::OpenGLES : HostDisplay::RenderAPI::OpenGL;
}

void LibretroOpenGLHostDisplay::SetVSync(bool enabled)
{
  // The libretro frontend controls this.
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

static bool TryDesktopVersions(retro_hw_render_callback* cb)
{
  static constexpr std::array<std::tuple<u32, u32>, 11> desktop_versions_to_try = {
    {/*{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, */ {3, 3}, {3, 2}, {3, 1}, {3, 0}}};

  for (const auto& [major, minor] : desktop_versions_to_try)
  {
    if (major > 3 || (major == 3 && minor >= 2))
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  return false;
}

static bool TryESVersions(retro_hw_render_callback* cb)
{
  static constexpr std::array<std::tuple<u32, u32>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  for (const auto& [major, minor] : es_versions_to_try)
  {
    if (major >= 3 && minor > 0)
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES_VERSION;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES3;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  return false;
}

bool LibretroOpenGLHostDisplay::RequestHardwareRendererContext(retro_hw_render_callback* cb, bool prefer_gles)
{
  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  cb->cache_context = false;
  cb->bottom_left_origin = true;

  if (!prefer_gles)
  {
    if (TryDesktopVersions(cb) || TryESVersions(cb))
      return true;
  }
  else
  {
    if (TryESVersions(cb) || TryDesktopVersions(cb))
      return true;
  }

  Log_ErrorPrint("Failed to set any GL HW renderer");
  return false;
}

bool LibretroOpenGLHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name,
                                                   bool debug_device, bool threaded_presentation)
{
  Assert(wi.type == WindowInfo::Type::Libretro);

  // gross - but can't do much because of the GLADloadproc below.
  static retro_hw_render_callback* cb;
  cb = static_cast<retro_hw_render_callback*>(wi.display_connection);

  m_window_info = wi;
  m_is_gles = (cb->context_type == RETRO_HW_CONTEXT_OPENGLES3 || cb->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION);

  const GLADloadproc get_proc_address = [](const char* sym) -> void* {
    return reinterpret_cast<void*>(cb->get_proc_address(sym));
  };

  // Load GLAD.
  const auto load_result = m_is_gles ? gladLoadGLES2Loader(get_proc_address) : gladLoadGLLoader(get_proc_address);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

  return true;
}

void LibretroOpenGLHostDisplay::DestroyRenderDevice()
{
  DestroyResources();
}

void LibretroOpenGLHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = static_cast<u32>(new_window_width);
  m_window_info.surface_height = static_cast<u32>(new_window_height);
}

bool LibretroOpenGLHostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  m_window_info = new_wi;
  return true;
}

bool LibretroOpenGLHostDisplay::Render()
{
  const GLuint fbo = static_cast<GLuint>(
    static_cast<retro_hw_render_callback*>(m_window_info.display_connection)->get_current_framebuffer());
  const u32 resolution_scale = g_libretro_host_interface.GetResolutionScale();
  const u32 display_width = static_cast<u32>(m_display_width) * resolution_scale;
  const u32 display_height = static_cast<u32>(m_display_height) * resolution_scale;

  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, display_width, display_height);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(display_width, display_height, 0, false);
    RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                  m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                  m_display_texture_view_height, m_display_linear_filtering);
  }

  if (HasSoftwareCursor())
  {
    // TODO: Scale mouse x/y
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect(m_mouse_position_x, m_mouse_position_y);
    RenderSoftwareCursor(left, display_height - top - height, width, height, m_cursor_texture.get());
  }

  g_retro_video_refresh_callback(RETRO_HW_FRAME_BUFFER_VALID, display_width, display_height, 0);

  GL::Program::ResetLastProgram();
  return true;
}
