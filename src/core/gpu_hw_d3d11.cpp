#include "gpu_hw_d3d11.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/log.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "host_interface.h"
#include "system.h"
Log_SetChannel(GPU_HW_D3D11);

GPU_HW_D3D11::GPU_HW_D3D11() = default;

GPU_HW_D3D11::~GPU_HW_D3D11()
{
  if (m_host_display)
  {
    m_host_display->ClearDisplayTexture();
    ResetGraphicsAPIState();
  }
}

bool GPU_HW_D3D11::Initialize(HostDisplay* host_display)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::D3D11)
  {
    Log_ErrorPrintf("Host render API is incompatible");
    return false;
  }

  SetCapabilities();

  if (!GPU_HW::Initialize(host_display))
    return false;

  m_device = static_cast<ID3D11Device*>(host_display->GetRenderDevice());
  m_context = static_cast<ID3D11DeviceContext*>(host_display->GetRenderContext());
  if (!m_device || !m_context)
    return false;

  m_shader_cache.Open(g_host_interface->GetShaderCacheBasePath(), m_device->GetFeatureLevel(),
                      g_settings.gpu_use_debug_device);

  if (!CreateFramebuffer())
  {
    Log_ErrorPrintf("Failed to create framebuffer");
    return false;
  }

  if (!CreateVertexBuffer())
  {
    Log_ErrorPrintf("Failed to create vertex buffer");
    return false;
  }

  if (!CreateUniformBuffer())
  {
    Log_ErrorPrintf("Failed to create uniform buffer");
    return false;
  }

  if (!CreateTextureBuffer())
  {
    Log_ErrorPrintf("Failed to create texture buffer");
    return false;
  }

  if (!CreateStateObjects())
  {
    Log_ErrorPrintf("Failed to create state objects");
    return false;
  }

  if (!CreateBatchInputLayout())
  {
    Log_ErrorPrintf("Failed to create batch input layout");
    return false;
  }

  if (!CompileShaders())
  {
    Log_ErrorPrintf("Failed to compile shaders");
    return false;
  }

  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_D3D11::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_D3D11::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  m_context->GSSetShader(nullptr, nullptr, 0);

  // In D3D11 we can't leave a buffer mapped across a Present() call.
  FlushRender();
}

void GPU_HW_D3D11::RestoreGraphicsAPIState()
{
  const UINT stride = sizeof(BatchVertex);
  const UINT offset = 0;
  m_context->IASetVertexBuffers(0, 1, m_vertex_stream_buffer.GetD3DBufferArray(), &stride, &offset);
  m_context->IASetInputLayout(m_batch_input_layout.Get());
  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->PSSetShaderResources(0, 1, m_vram_read_texture.GetD3DSRVArray());
  m_context->OMSetRenderTargets(1, m_vram_texture.GetD3DRTVArray(), m_vram_depth_view.Get());
  m_context->RSSetState(m_cull_none_rasterizer_state.Get());
  SetViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
  SetScissorFromDrawingArea();
  m_batch_ubo_dirty = true;
}

void GPU_HW_D3D11::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  CreateFramebuffer();
  CreateStateObjects();
  CompileShaders();
  RestoreGraphicsAPIState();
  UpdateDisplay();
}

void GPU_HW_D3D11::MapBatchVertexPointer(u32 required_vertices)
{
  DebugAssert(!m_batch_start_vertex_ptr);

  const D3D11::StreamBuffer::MappingResult res =
    m_vertex_stream_buffer.Map(m_context.Get(), sizeof(BatchVertex), required_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(res.pointer);
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + res.space_aligned;
  m_batch_base_vertex = res.index_aligned;
}

void GPU_HW_D3D11::UnmapBatchVertexPointer(u32 used_vertices)
{
  DebugAssert(m_batch_start_vertex_ptr);
  m_vertex_stream_buffer.Unmap(m_context.Get(), used_vertices * sizeof(BatchVertex));
  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;
}

void GPU_HW_D3D11::SetCapabilities()
{
  const u32 max_texture_size = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  const u32 max_texture_scale = max_texture_size / VRAM_WIDTH;

  m_max_resolution_scale = max_texture_scale;
  m_supports_dual_source_blend = true;
}

bool GPU_HW_D3D11::CreateFramebuffer()
{
  // save old vram texture/fbo, in case we're changing scale
  auto old_vram_texture = std::move(m_vram_texture);
  DestroyFramebuffer();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const DXGI_FORMAT texture_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const DXGI_FORMAT depth_format = DXGI_FORMAT_D16_UNORM;

  if (!m_vram_texture.Create(m_device.Get(), texture_width, texture_height, texture_format,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
      !m_vram_depth_texture.Create(m_device.Get(), texture_width, texture_height, depth_format,
                                   D3D11_BIND_DEPTH_STENCIL) ||
      !m_vram_read_texture.Create(m_device.Get(), texture_width, texture_height, texture_format,
                                  D3D11_BIND_SHADER_RESOURCE) ||
      !m_display_texture.Create(m_device.Get(), texture_width, texture_height, texture_format,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
      !m_vram_encoding_texture.Create(m_device.Get(), VRAM_WIDTH, VRAM_HEIGHT, texture_format,
                                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
      !m_vram_readback_texture.Create(m_device.Get(), VRAM_WIDTH, VRAM_HEIGHT, texture_format, false))
  {
    return false;
  }

  const CD3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc(D3D11_DSV_DIMENSION_TEXTURE2D, depth_format);
  HRESULT hr =
    m_device->CreateDepthStencilView(m_vram_depth_texture, &depth_view_desc, m_vram_depth_view.GetAddressOf());
  if (FAILED(hr))
    return false;

  // do we need to restore the framebuffer after a size change?
  if (old_vram_texture)
  {
    const bool linear_filter = old_vram_texture.GetWidth() > m_vram_texture.GetWidth();
    Log_DevPrintf("Scaling %ux%u VRAM texture to %ux%u using %s filter", old_vram_texture.GetWidth(),
                  old_vram_texture.GetHeight(), m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                  linear_filter ? "linear" : "nearest");

    BlitTexture(m_vram_texture.GetD3DRTV(), 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                old_vram_texture.GetD3DSRV(), 0, 0, old_vram_texture.GetWidth(), old_vram_texture.GetHeight(),
                old_vram_texture.GetWidth(), old_vram_texture.GetHeight(), linear_filter);
    UpdateDepthBufferFromMaskBit();
  }

  m_context->OMSetRenderTargets(1, m_vram_texture.GetD3DRTVArray(), nullptr);
  SetFullVRAMDirtyRectangle();
  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_D3D11::ClearFramebuffer()
{
  static constexpr std::array<float, 4> color = {};
  m_context->ClearRenderTargetView(m_vram_texture.GetD3DRTV(), color.data());
  m_context->ClearDepthStencilView(m_vram_depth_view.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
  m_context->ClearRenderTargetView(m_display_texture, color.data());
  SetFullVRAMDirtyRectangle();
}

void GPU_HW_D3D11::DestroyFramebuffer()
{
  m_vram_read_texture.Destroy();
  m_vram_depth_view.Reset();
  m_vram_depth_texture.Destroy();
  m_vram_texture.Destroy();
  m_vram_encoding_texture.Destroy();
  m_display_texture.Destroy();
  m_vram_readback_texture.Destroy();
}

bool GPU_HW_D3D11::CreateVertexBuffer()
{
  return m_vertex_stream_buffer.Create(m_device.Get(), D3D11_BIND_VERTEX_BUFFER, VERTEX_BUFFER_SIZE);
}

bool GPU_HW_D3D11::CreateUniformBuffer()
{
  return m_uniform_stream_buffer.Create(m_device.Get(), D3D11_BIND_CONSTANT_BUFFER, MAX_UNIFORM_BUFFER_SIZE);
}

bool GPU_HW_D3D11::CreateTextureBuffer()
{
  if (!m_texture_stream_buffer.Create(m_device.Get(), D3D11_BIND_SHADER_RESOURCE, VRAM_UPDATE_TEXTURE_BUFFER_SIZE))
    return false;

  const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R16_UINT, 0,
                                                  VRAM_UPDATE_TEXTURE_BUFFER_SIZE / sizeof(u16));
  const HRESULT hr = m_device->CreateShaderResourceView(m_texture_stream_buffer.GetD3DBuffer(), &srv_desc,
                                                        m_texture_stream_buffer_srv_r16ui.ReleaseAndGetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Creation of texture buffer SRV failed: 0x%08X", hr);
    return false;
  }

  return true;
}

bool GPU_HW_D3D11::CreateBatchInputLayout()
{
  static constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> attributes = {
    {{"ATTR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(BatchVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(BatchVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 2, DXGI_FORMAT_R32_UINT, 0, offsetof(BatchVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 3, DXGI_FORMAT_R32_UINT, 0, offsetof(BatchVertex, texpage), D3D11_INPUT_PER_VERTEX_DATA, 0}}};

  // we need a vertex shader...
  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_true_color, m_scaled_dithering,
                             m_texture_filtering, m_supports_dual_source_blend);
  ComPtr<ID3DBlob> vs_bytecode =
    m_shader_cache.GetShaderBlob(D3D11::ShaderCompiler::Type::Vertex, shadergen.GenerateBatchVertexShader(true, false));
  if (!vs_bytecode)
    return false;

  const HRESULT hr = m_device->CreateInputLayout(attributes.data(), static_cast<UINT>(attributes.size()),
                                                 vs_bytecode->GetBufferPointer(), vs_bytecode->GetBufferSize(),
                                                 m_batch_input_layout.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateInputLayout failed: 0x%08X", hr);
    return false;
  }

  return true;
}

bool GPU_HW_D3D11::CreateStateObjects()
{
  HRESULT hr;

  CD3D11_RASTERIZER_DESC rs_desc = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
  rs_desc.CullMode = D3D11_CULL_NONE;
  rs_desc.ScissorEnable = TRUE;
  hr = m_device->CreateRasterizerState(&rs_desc, m_cull_none_rasterizer_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_DEPTH_STENCIL_DESC ds_desc = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
  ds_desc.DepthEnable = FALSE;
  ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  hr = m_device->CreateDepthStencilState(&ds_desc, m_depth_disabled_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  ds_desc.DepthEnable = TRUE;
  ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  ds_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  hr = m_device->CreateDepthStencilState(&ds_desc, m_depth_test_always_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  ds_desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  hr = m_device->CreateDepthStencilState(&ds_desc, m_depth_test_less_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_BLEND_DESC bl_desc = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
  hr = m_device->CreateBlendState(&bl_desc, m_blend_disabled_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  bl_desc.RenderTarget[0].RenderTargetWriteMask = 0;
  hr = m_device->CreateBlendState(&bl_desc, m_blend_no_color_writes_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_SAMPLER_DESC sampler_desc = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_point_sampler_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_linear_sampler_state.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;

  for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
  {
    bl_desc = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
    if (transparency_mode != static_cast<u8>(TransparencyMode::Disabled) || m_texture_filtering)
    {
      bl_desc.RenderTarget[0].BlendEnable = TRUE;
      bl_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
      bl_desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_ALPHA;
      bl_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
      bl_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
      bl_desc.RenderTarget[0].BlendOp =
        (transparency_mode == static_cast<u8>(TransparencyMode::BackgroundMinusForeground)) ?
          D3D11_BLEND_OP_REV_SUBTRACT :
          D3D11_BLEND_OP_ADD;
      bl_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }

    hr = m_device->CreateBlendState(&bl_desc, m_batch_blend_states[transparency_mode].ReleaseAndGetAddressOf());
    if (FAILED(hr))
      return false;
  }

  return true;
}

bool GPU_HW_D3D11::CompileShaders()
{
  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_true_color, m_scaled_dithering,
                             m_texture_filtering, m_supports_dual_source_blend);

  g_host_interface->DisplayLoadingScreen("Compiling shaders...");

  m_screen_quad_vertex_shader =
    m_shader_cache.GetVertexShader(m_device.Get(), shadergen.GenerateScreenQuadVertexShader());
  if (!m_screen_quad_vertex_shader)
    return false;

  for (u8 textured = 0; textured < 2; textured++)
  {
    const std::string vs = shadergen.GenerateBatchVertexShader(ConvertToBoolUnchecked(textured), false);
    m_batch_vertex_shaders[textured] = m_shader_cache.GetVertexShader(m_device.Get(), vs);
    if (!m_batch_vertex_shaders[textured])
      return false;
  }

  for (u8 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        for (u8 interlacing = 0; interlacing < 2; interlacing++)
        {
          const std::string ps = shadergen.GenerateBatchFragmentShader(
            static_cast<BatchRenderMode>(render_mode), static_cast<TextureMode>(texture_mode),
            ConvertToBoolUnchecked(dithering), ConvertToBoolUnchecked(interlacing));

          m_batch_pixel_shaders[render_mode][texture_mode][dithering][interlacing] =
            m_shader_cache.GetPixelShader(m_device.Get(), ps);
          if (!m_batch_pixel_shaders[render_mode][texture_mode][dithering][interlacing])
            return false;
        }
      }
    }
  }

  m_copy_pixel_shader = m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateCopyFragmentShader());
  if (!m_copy_pixel_shader)
    return false;

  m_vram_fill_pixel_shader = m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateFillFragmentShader());
  if (!m_vram_fill_pixel_shader)
    return false;

  m_vram_interlaced_fill_pixel_shader =
    m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateInterlacedFillFragmentShader());
  if (!m_vram_interlaced_fill_pixel_shader)
    return false;

  m_vram_read_pixel_shader = m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateVRAMReadFragmentShader());
  if (!m_vram_read_pixel_shader)
    return false;

  m_vram_write_pixel_shader =
    m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateVRAMWriteFragmentShader(false));
  if (!m_vram_write_pixel_shader)
    return false;

  m_vram_copy_pixel_shader = m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateVRAMCopyFragmentShader());
  if (!m_vram_copy_pixel_shader)
    return false;

  m_vram_update_depth_pixel_shader =
    m_shader_cache.GetPixelShader(m_device.Get(), shadergen.GenerateVRAMUpdateDepthFragmentShader());
  if (!m_vram_update_depth_pixel_shader)
    return false;

  for (u8 depth_24bit = 0; depth_24bit < 2; depth_24bit++)
  {
    for (u8 interlacing = 0; interlacing < 3; interlacing++)
    {
      const std::string ps = shadergen.GenerateDisplayFragmentShader(ConvertToBoolUnchecked(depth_24bit),
                                                                     static_cast<InterlacedRenderMode>(interlacing));
      m_display_pixel_shaders[depth_24bit][interlacing] = m_shader_cache.GetPixelShader(m_device.Get(), ps);
      if (!m_display_pixel_shaders[depth_24bit][interlacing])
        return false;
    }
  }

  return true;
}

void GPU_HW_D3D11::UploadUniformBuffer(const void* data, u32 data_size)
{
  Assert(data_size <= MAX_UNIFORM_BUFFER_SIZE);

  const auto res = m_uniform_stream_buffer.Map(m_context.Get(), MAX_UNIFORM_BUFFER_SIZE, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_stream_buffer.Unmap(m_context.Get(), data_size);

  m_context->VSSetConstantBuffers(0, 1, m_uniform_stream_buffer.GetD3DBufferArray());
  m_context->PSSetConstantBuffers(0, 1, m_uniform_stream_buffer.GetD3DBufferArray());

  m_renderer_stats.num_uniform_buffer_updates++;
}

void GPU_HW_D3D11::SetViewport(u32 x, u32 y, u32 width, u32 height)
{
  const CD3D11_VIEWPORT vp(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                           static_cast<float>(height));
  m_context->RSSetViewports(1, &vp);
}

void GPU_HW_D3D11::SetScissor(u32 x, u32 y, u32 width, u32 height)
{
  const CD3D11_RECT rc(x, y, x + width, y + height);
  m_context->RSSetScissorRects(1, &rc);
}

void GPU_HW_D3D11::SetViewportAndScissor(u32 x, u32 y, u32 width, u32 height)
{
  SetViewport(x, y, width, height);
  SetScissor(x, y, width, height);
}

void GPU_HW_D3D11::BlitTexture(ID3D11RenderTargetView* dst, u32 dst_x, u32 dst_y, u32 dst_width, u32 dst_height,
                               ID3D11ShaderResourceView* src, u32 src_x, u32 src_y, u32 src_width, u32 src_height,
                               u32 src_texture_width, u32 src_texture_height, bool linear_filter)
{
  const float uniforms[4] = {static_cast<float>(src_x) / static_cast<float>(src_texture_width),
                             static_cast<float>(src_y) / static_cast<float>(src_texture_height),
                             static_cast<float>(src_width) / static_cast<float>(src_texture_width),
                             static_cast<float>(src_height) / static_cast<float>(src_texture_height)};

  m_context->OMSetRenderTargets(1, &dst, nullptr);
  m_context->OMSetDepthStencilState(m_depth_disabled_state.Get(), 0);
  m_context->PSSetShaderResources(0, 1, &src);
  m_context->PSSetSamplers(
    0, 1, linear_filter ? m_linear_sampler_state.GetAddressOf() : m_point_sampler_state.GetAddressOf());
  SetViewport(dst_x, dst_y, dst_width, dst_height);
  SetScissor(dst_x, dst_y, dst_width, dst_height);
  DrawUtilityShader(m_copy_pixel_shader.Get(), uniforms, sizeof(uniforms));
}

void GPU_HW_D3D11::DrawUtilityShader(ID3D11PixelShader* shader, const void* uniforms, u32 uniforms_size)
{
  if (uniforms)
  {
    UploadUniformBuffer(uniforms, uniforms_size);
    m_batch_ubo_dirty = true;
  }

  m_context->VSSetShader(m_screen_quad_vertex_shader.Get(), nullptr, 0);
  m_context->GSSetShader(nullptr, nullptr, 0);
  m_context->PSSetShader(shader, nullptr, 0);
  m_context->OMSetBlendState(m_blend_disabled_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);
}

void GPU_HW_D3D11::DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices)
{
  const bool textured = (m_batch.texture_mode != TextureMode::Disabled);

  m_context->VSSetShader(m_batch_vertex_shaders[BoolToUInt8(textured)].Get(), nullptr, 0);

  m_context->PSSetShader(m_batch_pixel_shaders[static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)]
                                              [BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)]
                                                .Get(),
                         nullptr, 0);

  const TransparencyMode transparency_mode =
    (render_mode == BatchRenderMode::OnlyOpaque) ? TransparencyMode::Disabled : m_batch.transparency_mode;
  m_context->OMSetBlendState(m_batch_blend_states[static_cast<u8>(transparency_mode)].Get(), nullptr, 0xFFFFFFFFu);
  m_context->OMSetDepthStencilState(
    m_batch.check_mask_before_draw ? m_depth_test_less_state.Get() : m_depth_test_always_state.Get(), 0);

  m_context->Draw(num_vertices, base_vertex);
}

void GPU_HW_D3D11::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  CD3D11_RECT rc(left, top, right, bottom);
  m_context->RSSetScissorRects(1, &rc);
}

void GPU_HW_D3D11::ClearDisplay()
{
  GPU_HW::ClearDisplay();

  static constexpr std::array<float, 4> clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
  m_context->ClearRenderTargetView(m_display_texture.GetD3DRTV(), clear_color.data());
}

void GPU_HW_D3D11::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (g_settings.debugging.show_vram)
  {
    m_host_display->SetDisplayTexture(m_vram_texture.GetD3DSRV(), m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                                      0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
  else
  {
    const u32 vram_offset_x = m_crtc_state.display_vram_left;
    const u32 vram_offset_y = m_crtc_state.display_vram_top;
    const u32 scaled_vram_offset_x = vram_offset_x * m_resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * m_resolution_scale;
    const u32 display_width = m_crtc_state.display_vram_width;
    const u32 display_height = m_crtc_state.display_vram_height;
    const u32 scaled_display_width = display_width * m_resolution_scale;
    const u32 scaled_display_height = display_height * m_resolution_scale;
    const InterlacedRenderMode interlaced = GetInterlacedRenderMode();

    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && interlaced == InterlacedRenderMode::None &&
             (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture.GetWidth() &&
             (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture.GetHeight())
    {
      m_host_display->SetDisplayTexture(m_vram_texture.GetD3DSRV(), m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), scaled_vram_offset_x, scaled_vram_offset_y,
                                        scaled_display_width, scaled_display_height);
    }
    else
    {
      m_context->OMSetRenderTargets(1, m_display_texture.GetD3DRTVArray(), nullptr);
      m_context->OMSetDepthStencilState(m_depth_disabled_state.Get(), 0);
      m_context->PSSetShaderResources(0, 1, m_vram_texture.GetD3DSRVArray());

      const u32 reinterpret_field_offset = (interlaced != InterlacedRenderMode::None) ? GetInterlacedDisplayField() : 0;
      const u32 reinterpret_start_x = m_crtc_state.regs.X * m_resolution_scale;
      const u32 reinterpret_crop_left = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * m_resolution_scale;
      const u32 uniforms[4] = {reinterpret_start_x, scaled_vram_offset_y + reinterpret_field_offset,
                               reinterpret_crop_left, reinterpret_field_offset};
      ID3D11PixelShader* display_pixel_shader =
        m_display_pixel_shaders[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)][static_cast<u8>(interlaced)].Get();

      SetViewportAndScissor(0, 0, scaled_display_width, scaled_display_height);
      DrawUtilityShader(display_pixel_shader, uniforms, sizeof(uniforms));

      m_host_display->SetDisplayTexture(m_display_texture.GetD3DSRV(), m_display_texture.GetWidth(),
                                        m_display_texture.GetHeight(), 0, 0, scaled_display_width,
                                        scaled_display_height);

      RestoreGraphicsAPIState();
    }

    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         m_crtc_state.display_aspect_ratio);
  }
}

void GPU_HW_D3D11::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  // Encode the 24-bit texture as 16-bit.
  const u32 uniforms[4] = {copy_rect.left, copy_rect.top, copy_rect.GetWidth(), copy_rect.GetHeight()};
  m_context->OMSetRenderTargets(1, m_vram_encoding_texture.GetD3DRTVArray(), nullptr);
  m_context->OMSetDepthStencilState(m_depth_disabled_state.Get(), 0);
  m_context->PSSetShaderResources(0, 1, m_vram_texture.GetD3DSRVArray());
  SetViewportAndScissor(0, 0, encoded_width, encoded_height);
  DrawUtilityShader(m_vram_read_pixel_shader.Get(), uniforms, sizeof(uniforms));

  // Stage the readback.
  m_vram_readback_texture.CopyFromTexture(m_context.Get(), m_vram_encoding_texture.GetD3DTexture(), 0, 0, 0, 0, 0,
                                          encoded_width, encoded_height);
  // And copy it into our shadow buffer.
  if (m_vram_readback_texture.Map(m_context.Get(), false))
  {
    m_vram_readback_texture.ReadPixels(0, 0, encoded_width * 2, encoded_height, VRAM_WIDTH,
                                       &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left]);
    m_vram_readback_texture.Unmap(m_context.Get());
  }
  else
  {
    Log_ErrorPrintf("Failed to map VRAM readback texture");
  }

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D11::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT)
  {
    // CPU round trip if oversized for now.
    Log_WarningPrintf("Oversized VRAM fill (%u-%u, %u-%u), CPU round trip", x, x + width, y, y + height);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GPU::FillVRAM(x, y, width, height, color);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data());
    return;
  }

  GPU_HW::FillVRAM(x, y, width, height, color);

  const VRAMFillUBOData uniforms = GetVRAMFillUBOData(x, y, width, height, color);

  m_context->OMSetDepthStencilState(m_depth_test_always_state.Get(), 0);

  SetViewportAndScissor(x * m_resolution_scale, y * m_resolution_scale, width * m_resolution_scale,
                        height * m_resolution_scale);
  DrawUtilityShader(IsInterlacedRenderingEnabled() ? m_vram_interlaced_fill_pixel_shader.Get() :
                                                     m_vram_fill_pixel_shader.Get(),
                    &uniforms, sizeof(uniforms));

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D11::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  const Common::Rectangle<u32> bounds = GetVRAMTransferBounds(x, y, width, height);
  GPU_HW::UpdateVRAM(bounds.left, bounds.top, bounds.GetWidth(), bounds.GetHeight(), data);

  const u32 num_pixels = width * height;
  const auto map_result = m_texture_stream_buffer.Map(m_context.Get(), sizeof(u16), num_pixels * sizeof(u16));
  std::memcpy(map_result.pointer, data, num_pixels * sizeof(u16));
  m_texture_stream_buffer.Unmap(m_context.Get(), num_pixels * sizeof(u16));

  const VRAMWriteUBOData uniforms = GetVRAMWriteUBOData(x, y, width, height, map_result.index_aligned);
  m_context->OMSetDepthStencilState(
    m_GPUSTAT.check_mask_before_draw ? m_depth_test_less_state.Get() : m_depth_test_always_state.Get(), 0);
  m_context->PSSetShaderResources(0, 1, m_texture_stream_buffer_srv_r16ui.GetAddressOf());

  // the viewport should already be set to the full vram, so just adjust the scissor
  const Common::Rectangle<u32> scaled_bounds = bounds * m_resolution_scale;
  SetScissor(scaled_bounds.left, scaled_bounds.top, scaled_bounds.GetWidth(), scaled_bounds.GetHeight());

  DrawUtilityShader(m_vram_write_pixel_shader.Get(), &uniforms, sizeof(uniforms));

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D11::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  if (UseVRAMCopyShader(src_x, src_y, dst_x, dst_y, width, height))
  {
    const Common::Rectangle<u32> src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
    const Common::Rectangle<u32> dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
    if (m_vram_dirty_rect.Intersects(src_bounds))
      UpdateVRAMReadTexture();
    IncludeVRAMDityRectangle(dst_bounds);

    const VRAMCopyUBOData uniforms = GetVRAMCopyUBOData(src_x, src_y, dst_x, dst_y, width, height);

    const Common::Rectangle<u32> dst_bounds_scaled(dst_bounds * m_resolution_scale);
    SetViewportAndScissor(dst_bounds_scaled.left, dst_bounds_scaled.top, dst_bounds_scaled.GetWidth(),
                          dst_bounds_scaled.GetHeight());
    m_context->OMSetDepthStencilState(
      m_GPUSTAT.check_mask_before_draw ? m_depth_test_less_state.Get() : m_depth_test_always_state.Get(), 0);
    m_context->PSSetShaderResources(0, 1, m_vram_read_texture.GetD3DSRVArray());
    DrawUtilityShader(m_vram_copy_pixel_shader.Get(), &uniforms, sizeof(uniforms));
    RestoreGraphicsAPIState();

    if (m_GPUSTAT.check_mask_before_draw)
      m_current_depth++;

    return;
  }

  // We can't CopySubresourceRegion to the same resource. So use the shadow texture if we can, but that may need to be
  // updated first. Copying to the same resource seemed to work on Windows 10, but breaks on Windows 7. But, it's
  // against the API spec, so better to be safe than sorry.
  if (m_vram_dirty_rect.Intersects(Common::Rectangle<u32>::FromExtents(src_x, src_y, width, height)))
    UpdateVRAMReadTexture();

  GPU_HW::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);

  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  const CD3D11_BOX src_box(src_x, src_y, 0, src_x + width, src_y + height, 1);
  m_context->CopySubresourceRegion(m_vram_texture, 0, dst_x, dst_y, 0, m_vram_read_texture, 0, &src_box);
}

void GPU_HW_D3D11::UpdateVRAMReadTexture()
{
  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;
  const CD3D11_BOX src_box(scaled_rect.left, scaled_rect.top, 0, scaled_rect.right, scaled_rect.bottom, 1);
  m_context->CopySubresourceRegion(m_vram_read_texture, 0, scaled_rect.left, scaled_rect.top, 0, m_vram_texture, 0,
                                   &src_box);

  GPU_HW::UpdateVRAMReadTexture();
}

void GPU_HW_D3D11::UpdateDepthBufferFromMaskBit()
{
  SetViewportAndScissor(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());

  m_context->OMSetRenderTargets(0, nullptr, m_vram_depth_view.Get());
  m_context->OMSetDepthStencilState(m_depth_test_always_state.Get(), 0);
  m_context->OMSetBlendState(m_blend_no_color_writes_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->PSSetShaderResources(0, 1, m_vram_texture.GetD3DSRVArray());
  DrawUtilityShader(m_vram_update_depth_pixel_shader.Get(), nullptr, 0);

  m_context->PSSetShaderResources(0, 1, m_vram_read_texture.GetD3DSRVArray());
  RestoreGraphicsAPIState();
}

std::unique_ptr<GPU> GPU::CreateHardwareD3D11Renderer() { return std::make_unique<GPU_HW_D3D11>(); }
