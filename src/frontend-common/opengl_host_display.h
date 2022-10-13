#pragma once
#include "common/gl/context.h"
#include "common/gl/loader.h"
#include "common/gl/program.h"
#include "common/gl/stream_buffer.h"
#include "common/gl/texture.h"
#include "common/timer.h"
#include "common/window_info.h"
#include "core/host_display.h"
#include "postprocessing_chain.h"
#include <memory>

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

  bool CreateRenderDevice(const WindowInfo& wi) override;
  bool InitializeRenderDevice() override;

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

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Format format, const void* data, u32 data_stride,
                                            bool dynamic = false) override;
  bool BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer, u32* out_pitch) override;
  void EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height) override;
  bool UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch) override;
  bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;
  bool SupportsTextureFormat(GPUTexture::Format format) const override;

  void SetVSync(bool enabled) override;

  bool Render(bool skip_present) override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        GPUTexture::Format* out_format) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  ALWAYS_INLINE GL::Context* GetGLContext() const { return m_gl_context.get(); }
  ALWAYS_INLINE bool UsePBOForUploads() const { return m_use_pbo_for_pixels; }
  ALWAYS_INLINE bool UseGLES3DrawPath() const { return m_use_gles2_draw_path; }
  ALWAYS_INLINE std::vector<u8>& GetTextureRepackBuffer() { return m_texture_repack_buffer; }

  GL::StreamBuffer* GetTextureStreamBuffer();

protected:
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool CreateResources() override;
  void DestroyResources() override;

  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  void RenderDisplay();
  void RenderImGui();
  void RenderSoftwareCursor();

  void RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, GL::Texture* texture, s32 texture_view_x,
                     s32 texture_view_y, s32 texture_view_width, s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 bottom, s32 width, s32 height, GPUTexture* texture_handle);

  struct PostProcessingStage
  {
    GL::Program program;
    GL::Texture output_texture;
    u32 uniforms_size;
  };

  bool CheckPostProcessingRenderTargets(u32 target_width, u32 target_height);
  void ApplyPostProcessingChain(GLuint final_target, s32 final_left, s32 final_top, s32 final_width, s32 final_height,
                                GL::Texture* texture, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                s32 texture_view_height, u32 target_width, u32 target_height);

  void CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void KickTimestampQuery();

  std::unique_ptr<GL::Context> m_gl_context;

  GL::Program m_display_program;
  GL::Program m_cursor_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;
  GLuint m_display_border_sampler = 0;
  GLuint m_uniform_buffer_alignment = 1;

  std::unique_ptr<GL::StreamBuffer> m_texture_stream_buffer;
  std::vector<u8> m_texture_repack_buffer;
  u32 m_texture_stream_buffer_offset = 0;

  FrontendCommon::PostProcessingChain m_post_processing_chain;
  GL::Texture m_post_processing_input_texture;
  std::unique_ptr<GL::StreamBuffer> m_post_processing_ubo;
  std::vector<PostProcessingStage> m_post_processing_stages;
  Common::Timer m_post_processing_timer;

  std::array<GLuint, NUM_TIMESTAMP_QUERIES> m_timestamp_queries = {};
  float m_accumulated_gpu_time = 0.0f;
  u8 m_read_timestamp_query = 0;
  u8 m_write_timestamp_query = 0;
  u8 m_waiting_timestamp_queries = 0;
  bool m_timestamp_query_started = false;

  bool m_use_gles2_draw_path = false;
  bool m_use_pbo_for_pixels = false;
};
