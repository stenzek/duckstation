#pragma once
#include "YBaseLib/Windows/WindowsHeaders.h"
#include "common/d3d11/stream_buffer.h"
#include "common/d3d11/texture.h"
#include "core/host_display.h"
#include <SDL.h>
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

class D3D11HostDisplay final : public HostDisplay
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11HostDisplay(SDL_Window* window);
  ~D3D11HostDisplay();

  static std::unique_ptr<HostDisplay> Create(SDL_Window* window);

  RenderAPI GetRenderAPI() const override;
  void* GetHostRenderDevice() const override;
  void* GetHostRenderContext() const override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;

  void SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height, u32 texture_width,
                         u32 texture_height, float aspect_ratio) override;
  void SetDisplayLinearFiltering(bool enabled) override;

  void SetVSync(bool enabled) override;

  std::tuple<u32, u32> GetWindowSize() const override;
  void WindowResized() override;

private:
  static constexpr u32 DISPLAY_UNIFORM_BUFFER_SIZE = 16;

  bool CreateD3DDevice();
  bool CreateD3DResources();
  bool CreateSwapChainRTV();
  bool CreateImGuiContext();

  void Render();
  void RenderDisplay();

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_gl_context = nullptr;
  int m_window_width = 0;
  int m_window_height = 0;

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;
  ComPtr<IDXGISwapChain> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;

  ComPtr<ID3D11RasterizerState> m_display_rasterizer_state;
  ComPtr<ID3D11DepthStencilState> m_display_depth_stencil_state;
  ComPtr<ID3D11BlendState> m_display_blend_state;
  ComPtr<ID3D11VertexShader> m_display_vertex_shader;
  ComPtr<ID3D11PixelShader> m_display_pixel_shader;
  ComPtr<ID3D11SamplerState> m_point_sampler;
  ComPtr<ID3D11SamplerState> m_linear_sampler;

  D3D11::Texture m_display_pixels_texture;
  D3D11::StreamBuffer m_display_uniform_buffer;

  ID3D11ShaderResourceView* m_display_srv = nullptr;
  s32 m_display_offset_x = 0;
  s32 m_display_offset_y = 0;
  s32 m_display_width = 0;
  s32 m_display_height = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  float m_display_aspect_ratio = 1.0f;

  bool m_display_texture_changed = false;
  bool m_display_linear_filtering = false;
  bool m_vsync = false;
};
