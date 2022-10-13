#pragma once
#include "core/host_display.h"
#include <string>

class RegTestHostDisplay final : public HostDisplay
{
public:
  RegTestHostDisplay();
  ~RegTestHostDisplay();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool HasRenderDevice() const override;
  bool HasRenderSurface() const override;

  bool CreateRenderDevice(const WindowInfo& wi) override;
  bool InitializeRenderDevice() override;

  bool MakeRenderContextCurrent() override;
  bool DoneRenderContextCurrent() override;

  bool ChangeRenderWindow(const WindowInfo& wi) override;
  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool SupportsFullscreen() const override;
  bool IsFullscreen() override;
  bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) override;
  void DestroyRenderSurface() override;

  bool SetPostProcessingChain(const std::string_view& config) override;

  bool CreateResources() override;
  void DestroyResources() override;

  AdapterAndModeList GetAdapterAndModeList() override;
  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Format format, const void* data, u32 data_stride,
                                                    bool dynamic = false) override;
  bool BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer, u32* out_pitch) override;
  void EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height) override;
  bool UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_stride) override;
  bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  void SetVSync(bool enabled) override;

  bool Render(bool skip_present) override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        GPUTexture::Format* out_format) override;

  bool SupportsTextureFormat(GPUTexture::Format format) const override;

private:

};

class RegTestTexture : public GPUTexture
{
public:
  RegTestTexture();
  ~RegTestTexture() override;

  ALWAYS_INLINE const std::vector<u32>& GetPixels() const { return m_frame_buffer; }
  ALWAYS_INLINE u32 GetPitch() const { return m_frame_buffer_pitch; }

  bool IsValid() const override;

  bool Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, GPUTexture::Format format);

  bool Upload(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_stride);
  bool Download(u32 x, u32 y, u32 width, u32 height, void* data, u32 data_stride) const;

  bool BeginUpload(u32 width, u32 height, void** out_buffer, u32* out_pitch);
  void EndUpload(u32 x, u32 y, u32 width, u32 height);

private:
  std::vector<u32> m_frame_buffer;
  std::vector<u32> m_staging_buffer;
  GPUTexture::Format m_frame_buffer_format = GPUTexture::Format::Unknown;
  u32 m_frame_buffer_pitch = 0;
};
