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
#include <wrl/client.h>

namespace FrontendCommon {

class D3D11HostDisplay final
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11HostDisplay();
  ~D3D11HostDisplay();

  ALWAYS_INLINE HostDisplay::RenderAPI GetRenderAPI() const { return HostDisplay::RenderAPI::D3D11; }
  ALWAYS_INLINE void* GetRenderDevice() const { return m_device.Get(); }
  ALWAYS_INLINE void* GetRenderContext() const { return m_context.Get(); }

  bool CreateContextAndSwapChain(const WindowInfo& wi, bool use_flip_model, bool debug_device);
  bool HasContext() const;
  void DestroyContext();

  bool CreateResources();
  void DestroyResources();

  bool CreateImGuiContext();
  void DestroyImGuiContext();

  ALWAYS_INLINE u32 GetSwapChainWidth() const { return m_swap_chain_width; }
  ALWAYS_INLINE u32 GetSwapChainHeight() const { return m_swap_chain_height; }
  ALWAYS_INLINE bool HasSwapChain() const { return static_cast<bool>(m_swap_chain); }

  bool RecreateSwapChain(const WindowInfo& new_wi, bool use_flip_model);
  void ResizeSwapChain(u32 new_width, u32 new_height);
  void DestroySwapChain();

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* initial_data,
                                                    u32 initial_data_stride, bool dynamic);
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride);
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride);

  void SetVSync(bool enabled);

  bool BeginRender();
  void RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                     u32 texture_height, u32 texture_view_x, u32 texture_view_y, u32 texture_view_width,
                     u32 texture_view_height, bool linear_filter);
  void RenderImGui();
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture_handle);
  void EndRenderAndPresent();

private:
  static constexpr u32 DISPLAY_UNIFORM_BUFFER_SIZE = 16;

  bool CreateSwapChain(const WindowInfo& new_wi, bool use_flip_model);
  bool CreateSwapChainRTV();

  ComPtr<IDXGIFactory> m_dxgi_factory;

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;
  ComPtr<IDXGISwapChain> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;

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

  u32 m_swap_chain_width = 0;
  u32 m_swap_chain_height = 0;

  bool m_allow_tearing_supported = false;
  bool m_using_flip_model_swap_chain = true;
  bool m_using_allow_tearing = false;
  bool m_vsync = true;
};

} // namespace FrontendCommon
