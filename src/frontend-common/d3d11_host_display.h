#pragma once
#include "common/d3d11/staging_texture.h"
#include "common/d3d11/stream_buffer.h"
#include "common/d3d11/texture.h"
#include "common/window_info.h"
#include "common/windows_headers.h"
#include "core/host_display.h"
#include "frontend-common/postprocessing_chain.h"
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

namespace FrontendCommon {

class D3D11HostDisplay final : public HostDisplay
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11HostDisplay();
  ~D3D11HostDisplay();

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

  bool GetHostRefreshRate(float* refresh_rate) override;

  void SetVSync(bool enabled) override;

  bool Render() override;
  bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                        HostDisplayPixelFormat* out_format) override;

  static AdapterAndModeList StaticGetAdapterAndModeList();

protected:
  static constexpr u32 DISPLAY_UNIFORM_BUFFER_SIZE = 16;

  static AdapterAndModeList GetAdapterAndModeList(IDXGIFactory* dxgi_factory);

  bool CreateResources() override;
  void DestroyResources() override;

  bool CreateImGuiContext() override;
  void DestroyImGuiContext() override;
  bool UpdateImGuiFontTexture() override;

  bool CreateSwapChain(const DXGI_MODE_DESC* fullscreen_mode);
  bool CreateSwapChainRTV();

  void RenderDisplay();
  void RenderSoftwareCursor();
  void RenderImGui();

  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     s32 texture_height, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                     s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture_handle);

  struct PostProcessingStage
  {
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    D3D11::Texture output_texture;
    u32 uniforms_size;
  };

  bool CheckPostProcessingRenderTargets(u32 target_width, u32 target_height);
  void ApplyPostProcessingChain(ID3D11RenderTargetView* final_target, s32 final_left, s32 final_top, s32 final_width,
                                s32 final_height, void* texture_handle, u32 texture_width, s32 texture_height,
                                s32 texture_view_x, s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                u32 target_width, u32 target_height);

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;

  ComPtr<IDXGIFactory> m_dxgi_factory;
  ComPtr<IDXGISwapChain> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;

  ComPtr<ID3D11RasterizerState> m_display_rasterizer_state;
  ComPtr<ID3D11DepthStencilState> m_display_depth_stencil_state;
  ComPtr<ID3D11BlendState> m_display_blend_state;
  ComPtr<ID3D11BlendState> m_software_cursor_blend_state;
  ComPtr<ID3D11VertexShader> m_display_vertex_shader;
  ComPtr<ID3D11PixelShader> m_display_pixel_shader;
  ComPtr<ID3D11PixelShader> m_display_alpha_pixel_shader;
  ComPtr<ID3D11SamplerState> m_point_sampler;
  ComPtr<ID3D11SamplerState> m_linear_sampler;

  D3D11::Texture m_display_pixels_texture;
  D3D11::StreamBuffer m_display_uniform_buffer;
  D3D11::AutoStagingTexture m_readback_staging_texture;

  bool m_allow_tearing_supported = false;
  bool m_using_flip_model_swap_chain = true;
  bool m_using_allow_tearing = false;
  bool m_vsync = true;

  PostProcessingChain m_post_processing_chain;
  D3D11::Texture m_post_processing_input_texture;
  std::vector<PostProcessingStage> m_post_processing_stages;
};

} // namespace FrontendCommon
