#pragma once
#include "common/d3d11/shader_cache.h"
#include "common/d3d11/staging_texture.h"
#include "common/d3d11/stream_buffer.h"
#include "common/d3d11/texture.h"
#include "gpu_hw.h"
#include <array>
#include <d3d11.h>
#include <memory>
#include <tuple>
#include <wrl/client.h>

class GPU_HW_D3D11 : public GPU_HW
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  GPU_HW_D3D11();
  ~GPU_HW_D3D11() override;

  bool Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                  Timers* timers) override;
  void Reset() override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

protected:
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void MapBatchVertexPointer(u32 required_vertices) override;
  void UpdateVRAMReadTexture() override;

private:
  void SetCapabilities();
  bool CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  bool CreateVertexBuffer();
  bool CreateUniformBuffer();
  bool CreateTextureBuffer();
  bool CreateBatchInputLayout();
  bool CreateStateObjects();

  bool CompileShaders();
  void SetDrawState(BatchRenderMode render_mode);
  void SetScissorFromDrawingArea();
  void UploadUniformBlock(const void* data, u32 data_size);
  void SetViewport(u32 x, u32 y, u32 width, u32 height);
  void SetScissor(u32 x, u32 y, u32 width, u32 height);
  void SetViewportAndScissor(u32 x, u32 y, u32 width, u32 height);

  /// Blits from src to dst, downscaling or upscaling in the process.
  void BlitTexture(ID3D11RenderTargetView* dst, u32 dst_x, u32 dst_y, u32 dst_width, u32 dst_height,
                   ID3D11ShaderResourceView* src, u32 src_x, u32 src_y, u32 src_width, u32 src_height,
                   u32 src_texture_width, u32 src_texture_height, bool linear_filter);

  void DrawUtilityShader(ID3D11PixelShader* shader, const void* uniforms, u32 uniforms_size);

  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;

  D3D11::ShaderCache m_shader_cache;

  // downsample texture - used for readbacks at >1xIR.
  D3D11::Texture m_vram_texture;
  D3D11::Texture m_vram_read_texture;
  D3D11::Texture m_vram_encoding_texture;
  D3D11::Texture m_display_texture;

  D3D11::StreamBuffer m_vertex_stream_buffer;

  D3D11::StreamBuffer m_uniform_stream_buffer;

  D3D11::StreamBuffer m_texture_stream_buffer;

  D3D11::StagingTexture m_vram_readback_texture;

  ComPtr<ID3D11ShaderResourceView> m_texture_stream_buffer_srv_r16ui;

  ComPtr<ID3D11RasterizerState> m_cull_none_rasterizer_state;

  ComPtr<ID3D11DepthStencilState> m_depth_disabled_state;

  ComPtr<ID3D11BlendState> m_blend_disabled_state;

  ComPtr<ID3D11SamplerState> m_point_sampler_state;
  ComPtr<ID3D11SamplerState> m_linear_sampler_state;

  std::array<ComPtr<ID3D11BlendState>, 5> m_batch_blend_states; // [transparency_mode]
  ComPtr<ID3D11InputLayout> m_batch_input_layout;
  std::array<ComPtr<ID3D11VertexShader>, 2> m_batch_vertex_shaders; // [textured]
  std::array<std::array<std::array<ComPtr<ID3D11PixelShader>, 2>, 9>, 4>
    m_batch_pixel_shaders; // [render_mode][texture_mode][dithering]
  ComPtr<ID3D11GeometryShader> m_batch_line_expand_geometry_shader;

  ComPtr<ID3D11VertexShader> m_screen_quad_vertex_shader;
  ComPtr<ID3D11PixelShader> m_copy_pixel_shader;
  ComPtr<ID3D11PixelShader> m_fill_pixel_shader;
  ComPtr<ID3D11PixelShader> m_vram_read_pixel_shader;
  ComPtr<ID3D11PixelShader> m_vram_write_pixel_shader;
  std::array<std::array<ComPtr<ID3D11PixelShader>, 2>, 2> m_display_pixel_shaders; // [depth_24][interlaced]
};
