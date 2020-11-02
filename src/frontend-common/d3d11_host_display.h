#pragma once
#include "common/d3d11/staging_texture.h"
#include "common/d3d11/stream_buffer.h"
#include "common/d3d11/texture.h"
#include "common/window_info.h"
#include "common/windows_headers.h"
#include "core/host_display.h"
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

#ifndef LIBRETRO
#include "frontend-common/postprocessing_chain.h"
#endif

namespace FrontendCommon {

class D3D11HostDisplay : public HostDisplay
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11HostDisplay();
  ~D3D11HostDisplay();

  virtual RenderAPI GetRenderAPI() const override;
  virtual void* GetRenderDevice() const override;
  virtual void* GetRenderContext() const override;

  virtual bool HasRenderDevice() const override;
  virtual bool HasRenderSurface() const override;

  virtual bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device) override;
  virtual bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device) override;
  virtual void DestroyRenderDevice() override;

  virtual bool MakeRenderContextCurrent() override;
  virtual bool DoneRenderContextCurrent() override;

  virtual bool ChangeRenderWindow(const WindowInfo& new_wi) override;
  virtual void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  virtual bool SupportsFullscreen() const override;
  virtual bool IsFullscreen() override;
  virtual bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) override;
  virtual void DestroyRenderSurface() override;

  virtual bool SetPostProcessingChain(const std::string_view& config) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* initial_data,
                                                    u32 initial_data_stride, bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  virtual void SetVSync(bool enabled) override;

  virtual bool Render() override;

#ifndef LIBRETRO
  struct AdapterInfo
  {
    std::vector<std::string> adapter_names;
    std::vector<std::string> fullscreen_modes;
  };
  static AdapterInfo GetAdapterInfo();
#endif

protected:
  static constexpr u32 DISPLAY_UNIFORM_BUFFER_SIZE = 16;

#ifndef LIBRETRO
  static AdapterInfo GetAdapterInfo(IDXGIFactory* dxgi_factory);
#endif

  virtual bool CreateResources() override;
  virtual void DestroyResources() override;

  virtual bool CreateImGuiContext();
  virtual void DestroyImGuiContext();

#ifndef LIBRETRO
  bool CreateSwapChain(const DXGI_MODE_DESC* fullscreen_mode);
  bool CreateSwapChainRTV();
#endif

  void RenderDisplay();
  void RenderSoftwareCursor();
  void RenderImGui();

  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     s32 texture_height, s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                     s32 texture_view_height, bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture_handle);

#ifndef LIBRETRO
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
                                s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                s32 texture_view_height);
#endif

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;

#ifndef LIBRETRO
  ComPtr<IDXGIFactory> m_dxgi_factory;
  ComPtr<IDXGISwapChain> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;
#endif

  ComPtr<ID3D11RasterizerState> m_display_rasterizer_state;
  ComPtr<ID3D11DepthStencilState> m_display_depth_stencil_state;
  ComPtr<ID3D11BlendState> m_display_blend_state;
  ComPtr<ID3D11BlendState> m_software_cursor_blend_state;
  ComPtr<ID3D11VertexShader> m_display_vertex_shader;
  ComPtr<ID3D11PixelShader> m_display_pixel_shader;
  ComPtr<ID3D11SamplerState> m_point_sampler;
  ComPtr<ID3D11SamplerState> m_linear_sampler;

  D3D11::Texture m_display_pixels_texture;
  D3D11::StreamBuffer m_display_uniform_buffer;
  D3D11::AutoStagingTexture m_readback_staging_texture;

#ifndef LIBRETRO
  bool m_allow_tearing_supported = false;
  bool m_using_flip_model_swap_chain = true;
  bool m_using_allow_tearing = false;
  bool m_vsync = true;

  PostProcessingChain m_post_processing_chain;
  D3D11::Texture m_post_processing_input_texture;
  std::vector<PostProcessingStage> m_post_processing_stages;
#endif
};

} // namespace FrontendCommon
