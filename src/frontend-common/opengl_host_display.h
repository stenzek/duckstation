#pragma once

// GLAD has to come first so that Qt doesn't pull in the system GL headers, which are incompatible with glad.
#include <glad.h>

// Hack to prevent Apple's glext.h headers from getting included via qopengl.h, since we still want to use glad.
#ifdef __APPLE__
#define __glext_h_
#endif

#include "common/gl/context.h"
#include "common/gl/program.h"
#include "common/gl/stream_buffer.h"
#include "common/gl/texture.h"
#include "common/window_info.h"
#include "core/host_display.h"
#include "postprocessing_chain.h"
#include <memory>

namespace FrontendCommon {

class OpenGLHostDisplay final : public HostDisplay
{
public:
  OpenGLHostDisplay();
  ~OpenGLHostDisplay();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool HasRenderDevice() const override;
  bool HasRenderSurface() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;
  bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                              bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  bool MakeRenderContextCurrent() override;
  bool DoneRenderContextCurrent() override;

  bool ChangeRenderWindow(const WindowInfo& new_wi) override;
  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool SupportsFullscreen() const override;
  bool IsFullscreen() override;
  bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) override;
  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroyRenderSurface() override;

  bool SetPostProcessingChain(const std::string_view& config) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    HostDisplayPixelFormat format, const void* data, u32 data_stride,
                                                    bool dynamic = false) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;
  bool DownloadTexture(const void* texture_handle, HostDisplayPixelFormat texture_format, u32 x, u32 y, u32 width,
                       u32 height, void* out_data, u32 out_data_stride) override;
  bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override;
  bool BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                             u32* out_pitch) override;
  void EndSetDisplayPixels() override;
  bool SetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, const void* buffer, u32 pitch) override;

  void SetVSync(bool enabled) override;

  bool Render() override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        HostDisplayPixelFormat* out_format) override;

protected:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool CreateResources() override;
  void DestroyResources() override;

  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  void BindDisplayPixelsTexture();
  void UpdateDisplayPixelsTextureFilter();

  void RenderDisplay();
  void RenderImGui();
  void RenderSoftwareCursor();

  void RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     s32 texture_height, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                     s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 bottom, s32 width, s32 height, HostDisplayTexture* texture_handle);

  struct PostProcessingStage
  {
    GL::Program program;
    GL::Texture output_texture;
    u32 uniforms_size;
  };

  bool CheckPostProcessingRenderTargets(u32 target_width, u32 target_height);
  void ApplyPostProcessingChain(GLuint final_target, s32 final_left, s32 final_top, s32 final_width, s32 final_height,
                                void* texture_handle, u32 texture_width, s32 texture_height, s32 texture_view_x,
                                s32 texture_view_y, s32 texture_view_width, s32 texture_view_height, u32 target_width,
                                u32 target_height);

  std::unique_ptr<GL::Context> m_gl_context;

  GL::Program m_display_program;
  GL::Program m_cursor_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;
  GLuint m_uniform_buffer_alignment = 1;

  GLuint m_display_pixels_texture_id = 0;
  std::unique_ptr<GL::StreamBuffer> m_display_pixels_texture_pbo;
  u32 m_display_pixels_texture_pbo_map_offset = 0;
  u32 m_display_pixels_texture_pbo_map_size = 0;
  std::vector<u8> m_gles_pixels_repack_buffer;

  PostProcessingChain m_post_processing_chain;
  GL::Texture m_post_processing_input_texture;
  std::unique_ptr<GL::StreamBuffer> m_post_processing_ubo;
  std::vector<PostProcessingStage> m_post_processing_stages;

  bool m_display_texture_is_linear_filtered = false;
  bool m_use_gles2_draw_path = false;
  bool m_use_pbo_for_pixels = false;
};

} // namespace FrontendCommon