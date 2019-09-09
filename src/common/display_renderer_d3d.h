#pragma once
#include "YBaseLib/Common.h"

#if defined(Y_COMPILER_MSVC)

#include "YBaseLib/Windows/WindowsHeaders.h"
#include "display_renderer.h"
#include <d3d11.h>
#include <memory>
#include <mutex>
#include <wrl.h>

class DisplayRendererD3D final : public DisplayRenderer
{
public:
  DisplayRendererD3D(WindowHandleType window_handle, u32 window_width, u32 window_height);
  ~DisplayRendererD3D();

  BackendType GetBackendType() override;

  std::unique_ptr<Display> CreateDisplay(const char* name, Display::Type type,
                                         u8 priority = Display::DEFAULT_PRIORITY) override;

  void WindowResized(u32 window_width, u32 window_height) override;

  bool BeginFrame() override;
  void RenderDisplays() override;
  void EndFrame() override;

  ID3D11Device* GetD3DDevice() const { return m_device.Get(); }
  ID3D11DeviceContext* GetD3DContext() const { return m_context.Get(); }
  ID3D11VertexShader* GetD3DVertexShader() const { return m_vertex_shader.Get(); }
  ID3D11PixelShader* GetD3DPixelShader() const { return m_pixel_shader.Get(); }
  ID3D11RasterizerState* GetD3DRasterizerState() const { return m_rasterizer_state.Get(); }
  ID3D11DepthStencilState* GetD3DDepthState() const { return m_depth_state.Get(); }
  ID3D11BlendState* GetD3DBlendState() const { return m_blend_state.Get(); }

protected:
  bool Initialize() override;

private:
  bool CreateRenderTargetView();

  Microsoft::WRL::ComPtr<ID3D11Device> m_device = nullptr;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context = nullptr;
  Microsoft::WRL::ComPtr<IDXGISwapChain> m_swap_chain = nullptr;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv = nullptr;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex_shader = nullptr;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel_shader = nullptr;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer_state = nullptr;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depth_state = nullptr;
  Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend_state = nullptr;
};

#endif
