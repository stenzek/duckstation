#pragma once
#include "common/windows_headers.h"
#include "common/d3d11/stream_buffer.h"
#include "common/d3d11/texture.h"
#include "core/host_display.h"
#include "qtdisplaywindow.h"
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

class D3D11DisplayWindow final : public QtDisplayWindow, private HostDisplay
{
  Q_OBJECT

public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11DisplayWindow(QtHostInterface* host_interface, QWindow* parent);
  ~D3D11DisplayWindow();

  HostDisplay* getHostDisplayInterface() override;

  bool createDeviceContext(QThread* worker_thread, bool debug_device) override;
  bool initializeDeviceContext(bool debug_device) override;
  void destroyDeviceContext() override;

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void* GetRenderWindow() const override;

  void ChangeRenderWindow(void* new_window) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;

  void SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height, u32 texture_width,
                         u32 texture_height, float aspect_ratio) override;
  void SetDisplayLinearFiltering(bool enabled) override;
  void SetDisplayTopMargin(int height) override;

  void SetVSync(bool enabled) override;

  std::tuple<u32, u32> GetWindowSize() const override;
  void WindowResized() override;

  void Render() override;

  void onWindowResized(int width, int height) override;

private:
  static constexpr u32 DISPLAY_UNIFORM_BUFFER_SIZE = 16;

  bool createImGuiContext() override;
  void destroyImGuiContext() override;
  bool createDeviceResources() override;
  void destroyDeviceResources() override;

  bool createSwapChainRTV();

  void renderDisplay();

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
  int m_display_top_margin = 0;
  float m_display_aspect_ratio = 1.0f;

  bool m_display_texture_changed = false;
  bool m_display_linear_filtering = false;
  bool m_allow_tearing_supported = false;
  bool m_vsync = false;
};
