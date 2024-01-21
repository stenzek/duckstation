// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "opengl_device.h"
#include "opengl_pipeline.h"
#include "opengl_stream_buffer.h"
#include "opengl_texture.h"

#include "core/host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>
#include <tuple>

Log_SetChannel(OpenGLDevice);

static constexpr std::array<float, 4> s_clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}};

OpenGLDevice::OpenGLDevice()
{
  // Something which won't be matched..
  std::memset(&m_last_rasterization_state, 0xFF, sizeof(m_last_rasterization_state));
  std::memset(&m_last_depth_state, 0xFF, sizeof(m_last_depth_state));
  std::memset(&m_last_blend_state, 0xFF, sizeof(m_last_blend_state));
  m_last_blend_state.enable = false;
  m_last_blend_state.constant = 0;
}

OpenGLDevice::~OpenGLDevice()
{
  Assert(!m_gl_context);
  Assert(!m_pipeline_disk_cache_file);
}

void OpenGLDevice::BindUpdateTextureUnit()
{
  GetInstance().SetActiveTexture(UPDATE_TEXTURE_UNIT - GL_TEXTURE0);
}

RenderAPI OpenGLDevice::GetRenderAPI() const
{
  return m_gl_context->IsGLES() ? RenderAPI::OpenGLES : RenderAPI::OpenGL;
}

std::unique_ptr<GPUTexture> OpenGLDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                        GPUTexture::Type type, GPUTexture::Format format,
                                                        const void* data, u32 data_stride)
{
  return OpenGLTexture::Create(width, height, layers, levels, samples, type, format, data, data_stride);
}

bool OpenGLDevice::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                   u32 out_data_stride)
{
  OpenGLTexture* T = static_cast<OpenGLTexture*>(texture);

  GLint alignment;
  if (out_data_stride & 1)
    alignment = 1;
  else if (out_data_stride & 2)
    alignment = 2;
  else
    alignment = 4;

  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / T->GetPixelSize());

  const auto [gl_internal_format, gl_format, gl_type] =
    OpenGLTexture::GetPixelFormatMapping(T->GetFormat(), m_gl_context->IsGLES());
  const u32 layer = 0;
  const u32 level = 0;

  s_stats.num_downloads++;

  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(T->GetGLId(), level, x, y, layer, width, height, 1, gl_format, gl_type,
                         height * out_data_stride, out_data);
  }
  else
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);

    if (T->GetLayers() > 1)
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, T->GetGLId(), level, layer);
    else
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, T->GetGLTarget(), T->GetGLId(), level);

    DebugAssert(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glReadPixels(x, y, width, height, gl_format, gl_type, out_data);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }

  return true;
}

bool OpenGLDevice::SupportsTextureFormat(GPUTexture::Format format) const
{
  const auto [gl_internal_format, gl_format, gl_type] =
    OpenGLTexture::GetPixelFormatMapping(format, m_gl_context->IsGLES());
  return (gl_internal_format != static_cast<GLenum>(0));
}

void OpenGLDevice::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                     GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                     u32 height)
{
  OpenGLTexture* D = static_cast<OpenGLTexture*>(dst);
  OpenGLTexture* S = static_cast<OpenGLTexture*>(src);
  CommitClear(D);
  CommitClear(S);

  s_stats.num_copies++;

  const GLuint sid = S->GetGLId();
  const GLuint did = D->GetGLId();
  if (GLAD_GL_VERSION_4_3 || GLAD_GL_ARB_copy_image)
  {
    glCopyImageSubData(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                       dst_y, dst_layer, width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                          dst_y, dst_layer, width, height, 1);
  }
  else if (GLAD_GL_OES_copy_image)
  {
    glCopyImageSubDataOES(sid, GL_TEXTURE_2D, src_level, src_x, src_y, src_layer, did, GL_TEXTURE_2D, dst_level, dst_x,
                          dst_y, dst_layer, width, height, 1);
  }
  else
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);
    if (D->IsTextureArray())
      glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, did, dst_level, dst_layer);
    else
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, D->GetGLTarget(), did, dst_level);
    if (S->IsTextureArray())
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sid, src_level, src_layer);
    else
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, S->GetGLTarget(), sid, src_level);

    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);

    if (m_current_fbo)
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_fbo);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    else
    {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }
}

void OpenGLDevice::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                        GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  OpenGLTexture* D = static_cast<OpenGLTexture*>(dst);
  OpenGLTexture* S = static_cast<OpenGLTexture*>(src);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);
  if (D->IsTextureArray())
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, D->GetGLId(), dst_level, dst_layer);
  else
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, D->GetGLTarget(), D->GetGLId(), dst_level);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, S->GetGLTarget(), S->GetGLId(), 0);

  CommitClear(S);
  if (width == D->GetMipWidth(dst_level) && height == D->GetMipHeight(dst_level))
  {
    D->SetState(GPUTexture::State::Dirty);
    if (glInvalidateFramebuffer)
    {
      const GLenum attachment = GL_COLOR_ATTACHMENT0;
      glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
    }
  }
  else
  {
    CommitClear(D);
  }

  s_stats.num_copies++;

  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glEnable(GL_SCISSOR_TEST);

  if (m_current_fbo)
  {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
  else
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
}

void OpenGLDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (const s32 idx = IsRenderTargetBound(t); idx >= 0)
    CommitRTClearInFB(static_cast<OpenGLTexture*>(t), static_cast<u32>(idx));
}

void OpenGLDevice::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (m_current_depth_target == t)
    CommitDSClearInFB(static_cast<OpenGLTexture*>(t));
}

void OpenGLDevice::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (t->IsRenderTarget())
  {
    if (const s32 idx = IsRenderTargetBound(t); idx >= 0)
      CommitRTClearInFB(static_cast<OpenGLTexture*>(t), static_cast<u32>(idx));
  }
  else
  {
    DebugAssert(t->IsDepthStencil());
    if (m_current_depth_target == t)
      CommitDSClearInFB(static_cast<OpenGLTexture*>(t));
  }
}

void OpenGLDevice::PushDebugGroup(const char* name)
{
#ifdef _DEBUG
  if (!glPushDebugGroup)
    return;

  glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(std::strlen(name)), name);
#endif
}

void OpenGLDevice::PopDebugGroup()
{
#ifdef _DEBUG
  if (!glPopDebugGroup)
    return;

  glPopDebugGroup();
#endif
}

void OpenGLDevice::InsertDebugMessage(const char* msg)
{
#ifdef _DEBUG
  if (!glDebugMessageInsert)
    return;

  if (msg[0] != '\0')
  {
    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0, GL_DEBUG_SEVERITY_NOTIFICATION,
                         static_cast<GLsizei>(std::strlen(msg)), msg);
  }
#endif
}

void OpenGLDevice::SetVSync(bool enabled)
{
  if (m_vsync_enabled == enabled)
    return;

  m_vsync_enabled = enabled;
  SetSwapInterval();
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrint(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrint(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OpenGLDevice::HasSurface() const
{
  return m_window_info.type != WindowInfo::Type::Surfaceless;
}

bool OpenGLDevice::CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                                std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                                Error* error)
{
  m_gl_context = GL::Context::Create(m_window_info, error);
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    m_gl_context.reset();
    return false;
  }

  // Is this needed?
  m_window_info = m_gl_context->GetWindowInfo();

  const bool opengl_is_available =
    ((!m_gl_context->IsGLES() && (GLAD_GL_VERSION_3_0 || GLAD_GL_ARB_uniform_buffer_object)) ||
     (m_gl_context->IsGLES() && GLAD_GL_ES_VERSION_3_1));
  if (!opengl_is_available)
  {
    Host::ReportErrorAsync(TRANSLATE_SV("GPUDevice", "Error"),
                           TRANSLATE_SV("GPUDevice", "OpenGL renderer unavailable, your driver or hardware is not "
                                                     "recent enough. OpenGL 3.1 or OpenGL ES 3.1 is required."));
    m_gl_context.reset();
    return false;
  }

  SetSwapInterval();
  if (HasSurface())
    RenderBlankFrame();

  if (m_debug_device && GLAD_GL_KHR_debug)
  {
    if (m_gl_context->IsGLES())
      glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    else
      glDebugMessageCallback(GLDebugCallback, nullptr);

    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }
  else
  {
    // Nail the function pointers so that we don't waste time calling them.
    glPushDebugGroup = nullptr;
    glPopDebugGroup = nullptr;
    glDebugMessageInsert = nullptr;
    glObjectLabel = nullptr;
  }

  bool buggy_pbo;
  if (!CheckFeatures(&buggy_pbo, disabled_features))
    return false;

  if (!CreateBuffers(buggy_pbo))
    return false;

  // Scissor test should always be enabled.
  glEnable(GL_SCISSOR_TEST);

  return true;
}

bool OpenGLDevice::CheckFeatures(bool* buggy_pbo, FeatureMask disabled_features)
{
  const bool is_gles = m_gl_context->IsGLES();

  bool vendor_id_amd = false;
  // bool vendor_id_nvidia = false;
  bool vendor_id_intel = false;
  bool vendor_id_arm = false;
  bool vendor_id_qualcomm = false;
  bool vendor_id_powervr = false;

  const char* vendor = (const char*)glGetString(GL_VENDOR);
  const char* renderer = (const char*)glGetString(GL_RENDERER);
  if (std::strstr(vendor, "Advanced Micro Devices") || std::strstr(vendor, "ATI Technologies Inc.") ||
      std::strstr(vendor, "ATI"))
  {
    Log_InfoPrint("AMD GPU detected.");
    vendor_id_amd = true;
  }
  else if (std::strstr(vendor, "NVIDIA Corporation"))
  {
    Log_InfoPrint("NVIDIA GPU detected.");
    // vendor_id_nvidia = true;
  }
  else if (std::strstr(vendor, "Intel"))
  {
    Log_InfoPrint("Intel GPU detected.");
    vendor_id_intel = true;
  }
  else if (std::strstr(vendor, "ARM"))
  {
    Log_InfoPrint("ARM GPU detected.");
    vendor_id_arm = true;
  }
  else if (std::strstr(vendor, "Qualcomm"))
  {
    Log_InfoPrint("Qualcomm GPU detected.");
    vendor_id_qualcomm = true;
  }
  else if (std::strstr(vendor, "Imagination Technologies") || std::strstr(renderer, "PowerVR"))
  {
    Log_InfoPrint("PowerVR GPU detected.");
    vendor_id_powervr = true;
  }

  // Don't use PBOs when we don't have ARB_buffer_storage, orphaning buffers probably ends up worse than just
  // using the normal texture update routines and letting the driver take care of it. PBOs are also completely
  // broken on mobile drivers.
  const bool is_shitty_mobile_driver = (vendor_id_powervr || vendor_id_qualcomm || vendor_id_arm);
  const bool is_buggy_pbo =
    (!GLAD_GL_VERSION_4_4 && !GLAD_GL_ARB_buffer_storage && !GLAD_GL_EXT_buffer_storage) || is_shitty_mobile_driver;
  *buggy_pbo = is_buggy_pbo;
  if (is_buggy_pbo && !is_shitty_mobile_driver)
    Log_WarningPrint("Not using PBOs for texture uploads because buffer_storage is unavailable.");

  GLint max_texture_size = 1024;
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  Log_DevFmt("GL_MAX_TEXTURE_SIZE: {}", max_texture_size);
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  Log_DevFmt("GL_MAX_SAMPLES: {}", max_samples);
  m_max_texture_size = std::max(1024u, static_cast<u32>(max_texture_size));
  m_max_multisamples = std::max(1u, static_cast<u32>(max_samples));

  GLint max_dual_source_draw_buffers = 0;
  glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &max_dual_source_draw_buffers);
  m_features.dual_source_blend =
    !(disabled_features & FEATURE_MASK_DUAL_SOURCE_BLEND) && (max_dual_source_draw_buffers > 0) &&
    (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended || GLAD_GL_EXT_blend_func_extended);

  m_features.framebuffer_fetch = !(disabled_features & FEATURE_MASK_FRAMEBUFFER_FETCH) &&
                                 (GLAD_GL_EXT_shader_framebuffer_fetch || GLAD_GL_ARM_shader_framebuffer_fetch);

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS's OpenGL driver.
  m_features.supports_texture_buffers = false;
#else
  m_features.supports_texture_buffers =
    !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS) && (GLAD_GL_VERSION_3_1 || GLAD_GL_ES_VERSION_3_2);

  // And Samsung's ANGLE/GLES driver?
  if (std::strstr(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), "ANGLE"))
    m_features.supports_texture_buffers = false;

  if (m_features.supports_texture_buffers)
  {
    GLint max_texel_buffer_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, reinterpret_cast<GLint*>(&max_texel_buffer_size));
    Log_DevFmt("GL_MAX_TEXTURE_BUFFER_SIZE: {}", max_texel_buffer_size);
    if (max_texel_buffer_size < static_cast<GLint>(MIN_TEXEL_BUFFER_ELEMENTS))
    {
      Log_WarningFmt("GL_MAX_TEXTURE_BUFFER_SIZE ({}) is below required minimum ({}), not using texture buffers.",
                     max_texel_buffer_size, MIN_TEXEL_BUFFER_ELEMENTS);
      m_features.supports_texture_buffers = false;
    }
  }
#endif

  if (!m_features.supports_texture_buffers && !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS))
  {
    // Try SSBOs.
    GLint max_fragment_storage_blocks = 0;
    GLint64 max_ssbo_size = 0;
    if (GLAD_GL_VERSION_4_3 || GLAD_GL_ES_VERSION_3_1 || GLAD_GL_ARB_shader_storage_buffer_object)
    {
      glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_fragment_storage_blocks);
      glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    }

    Log_DevFmt("GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS: {}", max_fragment_storage_blocks);
    Log_DevFmt("GL_MAX_SHADER_STORAGE_BLOCK_SIZE: {}", max_ssbo_size);
    m_features.texture_buffers_emulated_with_ssbo =
      (max_fragment_storage_blocks > 0 && max_ssbo_size >= static_cast<GLint64>(1024 * 512 * sizeof(u16)));
    if (m_features.texture_buffers_emulated_with_ssbo)
    {
      Log_InfoPrint("Using shader storage buffers for VRAM writes.");
      m_features.supports_texture_buffers = true;
    }
    else
    {
      Log_WarningPrint("Both texture buffers and SSBOs are not supported. Performance will suffer.");
    }
  }

  // Sample rate shading is broken on AMD and Intel.
  // If AMD and Intel can't get it right, I very much doubt broken mobile drivers can.
  m_features.per_sample_shading = (GLAD_GL_VERSION_4_0 || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_ARB_sample_shading) &&
                                  (!vendor_id_amd && !vendor_id_intel && !is_shitty_mobile_driver);

  // noperspective is not supported in GLSL ES.
  m_features.noperspective_interpolation = !is_gles;

  // glBlitFramebufer with same source/destination should be legal, but on Mali (at least Bifrost) it breaks.
  // So, blit from the shadow texture, like in the other renderers.
  m_features.texture_copy_to_self = !vendor_id_arm && !(disabled_features & FEATURE_MASK_TEXTURE_COPY_TO_SELF);

  m_features.geometry_shaders =
    !(disabled_features & FEATURE_MASK_GEOMETRY_SHADERS) && (GLAD_GL_VERSION_3_2 || GLAD_GL_ES_VERSION_3_2);

  m_features.gpu_timing = !(m_gl_context->IsGLES() &&
                            (!GLAD_GL_EXT_disjoint_timer_query || !glGetQueryObjectivEXT || !glGetQueryObjectui64vEXT));
  m_features.partial_msaa_resolve = true;

  m_features.shader_cache = false;

  m_features.pipeline_cache = m_gl_context->IsGLES() || GLAD_GL_ARB_get_program_binary;
  if (m_features.pipeline_cache)
  {
    // check that there's at least one format and the extension isn't being "faked"
    GLint num_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
    Log_DevFmt("{} program binary formats supported by driver", num_formats);
    m_features.pipeline_cache = (num_formats > 0);
  }

  if (!m_features.pipeline_cache)
  {
    Log_WarningPrintf("Your GL driver does not support program binaries. Hopefully it has a built-in cache, otherwise "
                      "startup will be slow due to compiling shaders.");
  }

  // Mobile drivers prefer textures to not be updated mid-frame.
  m_features.prefer_unused_textures = is_gles || vendor_id_arm || vendor_id_powervr || vendor_id_qualcomm;

  return true;
}

void OpenGLDevice::DestroyDevice()
{
  if (!m_gl_context)
    return;

  ClosePipelineCache();
  DestroyBuffers();

  m_gl_context->DoneCurrent();
  m_gl_context.reset();
}

bool OpenGLDevice::UpdateWindow()
{
  Assert(m_gl_context);

  DestroySurface();

  if (!AcquireWindow(false))
    return false;

  if (!m_gl_context->ChangeSurface(m_window_info))
  {
    Log_ErrorPrintf("Failed to change surface");
    return false;
  }

  m_window_info = m_gl_context->GetWindowInfo();

  if (m_window_info.type != WindowInfo::Type::Surfaceless)
  {
    // reset vsync rate, since it (usually) gets lost
    SetSwapInterval();
    RenderBlankFrame();
  }

  return true;
}

void OpenGLDevice::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  if (m_window_info.IsSurfaceless())
    return;

  m_window_info.surface_scale = new_window_scale;
  if (m_window_info.surface_width == static_cast<u32>(new_window_width) &&
      m_window_info.surface_height == static_cast<u32>(new_window_height))
  {
    return;
  }

  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_info = m_gl_context->GetWindowInfo();
}

std::string OpenGLDevice::GetDriverInfo() const
{
  const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const char* gl_shading_language_version = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  return fmt::format("OpenGL Context:\n{}\n{} {}\nGLSL: {}", gl_version, gl_vendor, gl_renderer,
                     gl_shading_language_version);
}

void OpenGLDevice::SetSwapInterval()
{
  if (m_window_info.type == WindowInfo::Type::Surfaceless)
    return;

  // Window framebuffer has to be bound to call SetSwapInterval.
  const s32 interval = m_vsync_enabled ? 1 : 0;
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  if (!m_gl_context->SetSwapInterval(interval))
    Log_WarningPrintf("Failed to set swap interval to %d", interval);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

void OpenGLDevice::RenderBlankFrame()
{
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearBufferfv(GL_COLOR, 0, s_clear_color.data());
  glColorMask(m_last_blend_state.write_r, m_last_blend_state.write_g, m_last_blend_state.write_b,
              m_last_blend_state.write_a);
  glEnable(GL_SCISSOR_TEST);
  m_gl_context->SwapBuffers();
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_fbo);
}

s32 OpenGLDevice::IsRenderTargetBound(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return static_cast<s32>(i);
  }

  return -1;
}

GLuint OpenGLDevice::CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags)
{
  glGetError();

  GLuint fbo_id;
  glGenFramebuffers(1, &fbo_id);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_id);

  for (u32 i = 0; i < num_rts; i++)
  {
    OpenGLTexture* const RT = static_cast<OpenGLTexture*>(rts[i]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, RT->GetGLTarget(), RT->GetGLId(), 0);
  }

  if (ds)
  {
    OpenGLTexture* const DS = static_cast<OpenGLTexture*>(ds);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, DS->GetGLTarget(), DS->GetGLId(), 0);
  }

  if (glGetError() != GL_NO_ERROR || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    Log_ErrorFmt("Failed to create GL framebuffer: {}", static_cast<s32>(glGetError()));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OpenGLDevice::GetInstance().m_current_fbo);
    glDeleteFramebuffers(1, &fbo_id);
    return {};
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OpenGLDevice::GetInstance().m_current_fbo);
  return fbo_id;
}

void OpenGLDevice::DestroyFramebuffer(GLuint fbo)
{
  if (fbo != 0)
    glDeleteFramebuffers(1, &fbo);
}

GPUDevice::AdapterAndModeList OpenGLDevice::GetAdapterAndModeList()
{
  AdapterAndModeList aml;

  if (m_gl_context)
  {
    for (const GL::Context::FullscreenModeInfo& fmi : m_gl_context->EnumerateFullscreenModes())
    {
      aml.fullscreen_modes.push_back(GetFullscreenModeString(fmi.width, fmi.height, fmi.refresh_rate));
    }
  }

  return aml;
}

void OpenGLDevice::DestroySurface()
{
  if (!m_gl_context)
    return;

  m_window_info.SetSurfaceless();
  if (!m_gl_context->ChangeSurface(m_window_info))
    Log_ErrorPrintf("Failed to switch to surfaceless");
}

bool OpenGLDevice::CreateBuffers(bool buggy_pbo)
{
  if (!(m_vertex_buffer = OpenGLStreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE)) ||
      !(m_index_buffer = OpenGLStreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE)) ||
      !(m_uniform_buffer = OpenGLStreamBuffer::Create(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE)))
  {
    Log_ErrorPrintf("Failed to create one or more device buffers.");
    return false;
  }

  GL_OBJECT_NAME(m_vertex_buffer, "Device Vertex Buffer");
  GL_OBJECT_NAME(m_index_buffer, "Device Index Buffer");
  GL_OBJECT_NAME(m_uniform_buffer, "Device Uniform Buffer");

  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

  if (!buggy_pbo)
  {
    if (!(m_texture_stream_buffer = OpenGLStreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, TEXTURE_STREAM_BUFFER_SIZE)))
    {
      Log_ErrorPrintf("Failed to create texture stream buffer");
      return false;
    }

    // Need to unbind otherwise normal uploads will fail.
    m_texture_stream_buffer->Unbind();

    GL_OBJECT_NAME(m_texture_stream_buffer, "Device Texture Stream Buffer");
  }

  GLuint fbos[2];
  glGetError();
  glGenFramebuffers(static_cast<GLsizei>(std::size(fbos)), fbos);
  if (const GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create framebuffers: %u", err);
    return false;
  }
  m_read_fbo = fbos[0];
  m_write_fbo = fbos[1];

  return true;
}

void OpenGLDevice::DestroyBuffers()
{
  if (m_write_fbo != 0)
    glDeleteFramebuffers(1, &m_write_fbo);
  if (m_read_fbo != 0)
    glDeleteFramebuffers(1, &m_read_fbo);
  m_texture_stream_buffer.reset();
  m_uniform_buffer.reset();
  m_index_buffer.reset();
  m_vertex_buffer.reset();
}

bool OpenGLDevice::BeginPresent(bool skip_present)
{
  if (skip_present || m_window_info.type == WindowInfo::Type::Surfaceless)
  {
    if (!skip_present)
    {
      glFlush();
      TrimTexturePool();
    }

    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearBufferfv(GL_COLOR, 0, s_clear_color.data());
  glColorMask(m_last_blend_state.write_r, m_last_blend_state.write_g, m_last_blend_state.write_b,
              m_last_blend_state.write_a);
  glEnable(GL_SCISSOR_TEST);

  const Common::Rectangle<s32> window_rc =
    Common::Rectangle<s32>::FromExtents(0, 0, m_window_info.surface_width, m_window_info.surface_height);

  m_current_fbo = 0;
  m_num_current_render_targets = 0;
  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_current_depth_target = nullptr;

  m_last_viewport = window_rc;
  m_last_scissor = window_rc;
  UpdateViewport();
  UpdateScissor();
  return true;
}

void OpenGLDevice::EndPresent()
{
  DebugAssert(m_current_fbo == 0);

  if (m_gpu_timing_enabled)
    PopTimestampQuery();

  m_gl_context->SwapBuffers();

  if (m_gpu_timing_enabled)
    KickTimestampQuery();

  TrimTexturePool();
}

void OpenGLDevice::CreateTimestampQueries()
{
  const bool gles = m_gl_context->IsGLES();
  const auto GenQueries = gles ? glGenQueriesEXT : glGenQueries;

  GenQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  KickTimestampQuery();
}

void OpenGLDevice::DestroyTimestampQueries()
{
  if (m_timestamp_queries[0] == 0)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto DeleteQueries = gles ? glDeleteQueriesEXT : glDeleteQueries;

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);
  }

  DeleteQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  m_timestamp_queries.fill(0);
  m_read_timestamp_query = 0;
  m_write_timestamp_query = 0;
  m_waiting_timestamp_queries = 0;
  m_timestamp_query_started = false;
}

void OpenGLDevice::PopTimestampQuery()
{
  const bool gles = m_gl_context->IsGLES();

  if (gles)
  {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint)
    {
      Log_VerbosePrintf("GPU timing disjoint, resetting.");
      if (m_timestamp_query_started)
        glEndQueryEXT(GL_TIME_ELAPSED);

      m_read_timestamp_query = 0;
      m_write_timestamp_query = 0;
      m_waiting_timestamp_queries = 0;
      m_timestamp_query_started = false;
    }
  }

  while (m_waiting_timestamp_queries > 0)
  {
    const auto GetQueryObjectiv = gles ? glGetQueryObjectivEXT : glGetQueryObjectiv;
    const auto GetQueryObjectui64v = gles ? glGetQueryObjectui64vEXT : glGetQueryObjectui64v;

    GLint available = 0;
    GetQueryObjectiv(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
      break;

    u64 result = 0;
    GetQueryObjectui64v(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT, &result);
    m_accumulated_gpu_time += static_cast<float>(static_cast<double>(result) / 1000000.0);
    m_read_timestamp_query = (m_read_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_waiting_timestamp_queries--;
  }

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);

    m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_timestamp_query_started = false;
    m_waiting_timestamp_queries++;
  }
}

void OpenGLDevice::KickTimestampQuery()
{
  if (m_timestamp_query_started || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto BeginQuery = gles ? glBeginQueryEXT : glBeginQuery;

  BeginQuery(GL_TIME_ELAPSED, m_timestamp_queries[m_write_timestamp_query]);
  m_timestamp_query_started = true;
}

bool OpenGLDevice::SetGPUTimingEnabled(bool enabled)
{
  if (m_gpu_timing_enabled == enabled)
    return true;
  else if (!m_features.gpu_timing)
    return false;

  m_gpu_timing_enabled = enabled;
  if (m_gpu_timing_enabled)
    CreateTimestampQueries();
  else
    DestroyTimestampQueries();

  return true;
}

float OpenGLDevice::GetAndResetAccumulatedGPUTime()
{
  const float value = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return value;
}

void OpenGLDevice::SetActiveTexture(u32 slot)
{
  if (m_last_texture_unit != slot)
  {
    m_last_texture_unit = slot;
    glActiveTexture(GL_TEXTURE0 + slot);
  }
}

void OpenGLDevice::UnbindTexture(GLuint id)
{
  for (u32 slot = 0; slot < MAX_TEXTURE_SAMPLERS; slot++)
  {
    auto& ss = m_last_samplers[slot];
    if (ss.first == id)
    {
      ss.first = 0;

      const GLenum unit = GL_TEXTURE0 + slot;
      if (m_last_texture_unit != unit)
      {
        m_last_texture_unit = unit;
        glActiveTexture(unit);
      }

      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
}

void OpenGLDevice::UnbindTexture(OpenGLTexture* tex)
{
  UnbindTexture(tex->GetGLId());

  if (tex->IsRenderTarget())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        Log_WarningPrint("Unbinding current RT");
        SetRenderTargets(nullptr, 0, m_current_depth_target);
        break;
      }
    }

    m_framebuffer_manager.RemoveRTReferences(tex);
  }
  else if (tex->IsDepthStencil())
  {
    if (m_current_depth_target == tex)
    {
      Log_WarningPrint("Unbinding current DS");
      SetRenderTargets(nullptr, 0, nullptr);
    }

    m_framebuffer_manager.RemoveDSReferences(tex);
  }
}

void OpenGLDevice::UnbindSSBO(GLuint id)
{
  if (m_last_ssbo != id)
    return;

  m_last_ssbo = 0;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
}

void OpenGLDevice::UnbindSampler(GLuint id)
{
  for (u32 slot = 0; slot < MAX_TEXTURE_SAMPLERS; slot++)
  {
    auto& ss = m_last_samplers[slot];
    if (ss.second == id)
    {
      ss.second = 0;
      glBindSampler(slot, 0);
    }
  }
}

void OpenGLDevice::UnbindPipeline(const OpenGLPipeline* pl)
{
  if (m_current_pipeline == pl)
  {
    m_current_pipeline = nullptr;
    glUseProgram(0);
  }
}

ALWAYS_INLINE_RELEASE void OpenGLDevice::SetVertexBufferOffsets(u32 base_vertex)
{
  const OpenGLPipeline::VertexArrayCacheKey& va = m_last_vao->first;
  const size_t stride = va.vertex_attribute_stride;
  for (u32 i = 0; i < va.num_vertex_attributes; i++)
  {
    glBindVertexBuffer(i, m_vertex_buffer->GetGLBufferId(), base_vertex * stride + va.vertex_attributes[i].offset,
                       static_cast<GLsizei>(stride));
  }
}

void OpenGLDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  s_stats.num_draws++;

  if (glDrawElementsBaseVertex) [[likely]]
  {
    glDrawArrays(m_current_pipeline->GetTopology(), base_vertex, vertex_count);
    return;
  }

  SetVertexBufferOffsets(base_vertex);
  glDrawArrays(m_current_pipeline->GetTopology(), 0, vertex_count);
}

void OpenGLDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  s_stats.num_draws++;

  if (glDrawElementsBaseVertex) [[likely]]
  {
    const void* indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(base_index) * sizeof(u16));
    glDrawElementsBaseVertex(m_current_pipeline->GetTopology(), index_count, GL_UNSIGNED_SHORT, indices, base_vertex);
    return;
  }

  SetVertexBufferOffsets(base_vertex);

  const void* indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(base_index) * sizeof(u16));
  glDrawElements(m_current_pipeline->GetTopology(), index_count, GL_UNSIGNED_SHORT, indices);
}

void OpenGLDevice::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                   u32* map_base_vertex)
{
  const auto res = m_vertex_buffer->Map(vertex_size, vertex_size * vertex_count);
  *map_ptr = res.pointer;
  *map_space = res.space_aligned;
  *map_base_vertex = res.index_aligned;
}

void OpenGLDevice::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  const u32 size = vertex_size * vertex_count;
  s_stats.buffer_streamed += size;
  m_vertex_buffer->Unmap(size);
}

void OpenGLDevice::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const auto res = m_index_buffer->Map(sizeof(DrawIndex), sizeof(DrawIndex) * index_count);
  *map_ptr = static_cast<DrawIndex*>(res.pointer);
  *map_space = res.space_aligned;
  *map_base_index = res.index_aligned;
}

void OpenGLDevice::UnmapIndexBuffer(u32 used_index_count)
{
  const u32 size = sizeof(DrawIndex) * used_index_count;
  s_stats.buffer_streamed += size;
  m_index_buffer->Unmap(size);
}

void OpenGLDevice::PushUniformBuffer(const void* data, u32 data_size)
{
  const auto res = m_uniform_buffer->Map(m_uniform_buffer_alignment, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_buffer->Unmap(data_size);
  s_stats.buffer_streamed += data_size;
  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_buffer->GetGLBufferId(), res.buffer_offset, data_size);
}

void* OpenGLDevice::MapUniformBuffer(u32 size)
{
  const auto res = m_uniform_buffer->Map(m_uniform_buffer_alignment, size);
  return res.pointer;
}

void OpenGLDevice::UnmapUniformBuffer(u32 size)
{
  const u32 pos = m_uniform_buffer->Unmap(size);
  s_stats.buffer_streamed += pos;
  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_buffer->GetGLBufferId(), pos, size);
}

void OpenGLDevice::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds)
{
  bool changed = (m_num_current_render_targets != num_rts || m_current_depth_target != ds);
  bool needs_ds_clear = (ds && ds->IsClearedOrInvalidated());
  bool needs_rt_clear = false;

  m_current_depth_target = static_cast<OpenGLTexture*>(ds);
  for (u32 i = 0; i < num_rts; i++)
  {
    OpenGLTexture* const dt = static_cast<OpenGLTexture*>(rts[i]);
    changed |= m_current_render_targets[i] != dt;
    m_current_render_targets[i] = dt;
    needs_rt_clear |= dt->IsClearedOrInvalidated();
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;
  if (changed)
  {
    GLuint fbo = 0;
    if (m_num_current_render_targets > 0 || m_current_depth_target)
    {
      if ((fbo = m_framebuffer_manager.Lookup(rts, num_rts, ds, 0)) == 0)
      {
        Log_ErrorFmt("Failed to get FBO for {} render targets", num_rts);
        m_current_fbo = 0;
        std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
        m_num_current_render_targets = 0;
        m_current_depth_target = nullptr;
        return;
      }
    }

    s_stats.num_render_passes++;
    m_current_fbo = fbo;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  }

  if (needs_rt_clear)
  {
    for (u32 i = 0; i < num_rts; i++)
    {
      OpenGLTexture* const dt = static_cast<OpenGLTexture*>(rts[i]);
      if (dt->IsClearedOrInvalidated())
        CommitRTClearInFB(dt, i);
    }
  }

  if (needs_ds_clear)
    CommitDSClearInFB(static_cast<OpenGLTexture*>(ds));
}

void OpenGLDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  DebugAssert(slot < MAX_TEXTURE_SAMPLERS);
  auto& sslot = m_last_samplers[slot];

  OpenGLTexture* T = static_cast<OpenGLTexture*>(texture);
  GLuint Tid;
  if (T)
  {
    Tid = T->GetGLId();
    CommitClear(T);
  }
  else
  {
    Tid = 0;
  }

  if (sslot.first != Tid)
  {
    sslot.first = Tid;

    SetActiveTexture(slot);
    glBindTexture(T ? T->GetGLTarget() : GL_TEXTURE_2D, T ? T->GetGLId() : 0);
  }

  const GLuint Sid = sampler ? static_cast<const OpenGLSampler*>(sampler)->GetID() : 0;
  if (sslot.second != Sid)
  {
    sslot.second = Sid;
    glBindSampler(slot, Sid);
  }
}

void OpenGLDevice::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  const OpenGLTextureBuffer* B = static_cast<const OpenGLTextureBuffer*>(buffer);
  if (!m_features.texture_buffers_emulated_with_ssbo)
  {
    const GLuint Tid = B ? B->GetTextureId() : 0;
    if (m_last_samplers[slot].first != Tid)
    {
      m_last_samplers[slot].first = Tid;
      SetActiveTexture(slot);
      glBindTexture(GL_TEXTURE_BUFFER, Tid);
    }
  }
  else
  {
    DebugAssert(slot == 0);
    const GLuint bid = B ? B->GetBuffer()->GetGLBufferId() : 0;
    if (m_last_ssbo == bid)
      return;

    m_last_ssbo = bid;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, bid);
  }
}

void OpenGLDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_last_viewport == rc)
    return;

  m_last_viewport = rc;
  UpdateViewport();
}

void OpenGLDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_last_scissor == rc)
    return;

  m_last_scissor = rc;
  UpdateScissor();
}

void OpenGLDevice::UpdateViewport()
{
  glViewport(m_last_viewport.left, m_last_viewport.top, m_last_viewport.GetWidth(), m_last_viewport.GetHeight());
}

void OpenGLDevice::UpdateScissor()
{
  glScissor(m_last_scissor.left, m_last_scissor.top, m_last_scissor.GetWidth(), m_last_scissor.GetHeight());
}
