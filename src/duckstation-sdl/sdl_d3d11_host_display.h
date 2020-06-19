#pragma once
#include "core/host_display.h"
#include "frontend-common/d3d11_host_display.h"
#include <SDL.h>

class SDLD3D11HostDisplay final : public HostDisplay
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  SDLD3D11HostDisplay(SDL_Window* window);
  ~SDLD3D11HostDisplay();

  static std::unique_ptr<HostDisplay> Create(SDL_Window* window, std::string_view adapter_name, bool debug_device);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void WindowResized(s32 new_window_width, s32 new_window_height) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  void SetVSync(bool enabled) override;

  void Render() override;

private:
  SDL_Window* m_window = nullptr;

  FrontendCommon::D3D11HostDisplay m_interface;

  bool Initialize(std::string_view adapter_name, bool debug_device);
};
