#pragma once
#include "common/d3d12/descriptor_heap_manager.h"
#include "common/d3d12/staging_texture.h"
#include "common/d3d12/stream_buffer.h"
#include "common/d3d12/texture.h"
#include "common/timer.h"
#include "common/window_info.h"
#include "common/windows_headers.h"
#include "core/host_display.h"
#include "postprocessing_chain.h"
#include <d3d12.h>
#include <dxgi.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

class D3D12HostDisplay final : public HostDisplay
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D12HostDisplay();
  ~D3D12HostDisplay();

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

  bool GetHostRefreshRate(float* refresh_rate) override;

  void SetVSync(bool enabled) override;

  bool Render(bool skip_present) override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        GPUTexture::Format* out_format) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  static AdapterAndModeList StaticGetAdapterAndModeList();

protected:
  struct PostProcessingStage
  {
    PostProcessingStage() = default;
    PostProcessingStage(PostProcessingStage&& move);
    ~PostProcessingStage();

    ComPtr<ID3D12PipelineState> pipeline;
    D3D12::Texture output_texture;
    u32 uniforms_size = 0;
  };

  static AdapterAndModeList GetAdapterAndModeList(IDXGIFactory* dxgi_factory);

  virtual bool CreateResources() override;
  virtual void DestroyResources() override;

  virtual bool CreateImGuiContext() override;
  virtual void DestroyImGuiContext() override;
  virtual bool UpdateImGuiFontTexture() override;

  bool CreateSwapChain(const DXGI_MODE_DESC* fullscreen_mode);
  bool CreateSwapChainRTV();
  void DestroySwapChainRTVs();

  void RenderDisplay(ID3D12GraphicsCommandList* cmdlist, D3D12::Texture* swap_chain_buf);
  void RenderSoftwareCursor(ID3D12GraphicsCommandList* cmdlist);
  void RenderImGui(ID3D12GraphicsCommandList* cmdlist);

  void RenderDisplay(ID3D12GraphicsCommandList* cmdlist, s32 left, s32 top, s32 width, s32 height,
                     D3D12::Texture* texture, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                     s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(ID3D12GraphicsCommandList* cmdlist, s32 left, s32 top, s32 width, s32 height,
                            GPUTexture* texture_handle);

  bool CheckPostProcessingRenderTargets(u32 target_width, u32 target_height);
  void ApplyPostProcessingChain(ID3D12GraphicsCommandList* cmdlist, D3D12::Texture* final_target, s32 final_left,
                                s32 final_top, s32 final_width, s32 final_height, D3D12::Texture* texture,
                                s32 texture_view_x, s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                u32 target_width, u32 target_height);

  ComPtr<IDXGIFactory> m_dxgi_factory;
  ComPtr<IDXGISwapChain> m_swap_chain;
  std::vector<D3D12::Texture> m_swap_chain_buffers;
  u32 m_current_swap_chain_buffer = 0;

  ComPtr<ID3D12RootSignature> m_display_root_signature;
  ComPtr<ID3D12PipelineState> m_display_pipeline;
  ComPtr<ID3D12PipelineState> m_software_cursor_pipeline;
  D3D12::DescriptorHandle m_point_sampler;
  D3D12::DescriptorHandle m_linear_sampler;
  D3D12::DescriptorHandle m_border_sampler;

  D3D12::Texture m_display_pixels_texture;
  D3D12::StagingTexture m_readback_staging_texture;

  ComPtr<ID3D12RootSignature> m_post_processing_root_signature;
  ComPtr<ID3D12RootSignature> m_post_processing_cb_root_signature;
  FrontendCommon::PostProcessingChain m_post_processing_chain;
  D3D12::StreamBuffer m_post_processing_cbuffer;
  D3D12::Texture m_post_processing_input_texture;
  std::vector<PostProcessingStage> m_post_processing_stages;
  Common::Timer m_post_processing_timer;

  bool m_allow_tearing_supported = false;
  bool m_using_allow_tearing = false;
  bool m_vsync = true;
};
