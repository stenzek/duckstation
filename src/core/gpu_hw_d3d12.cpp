#include "gpu_hw_d3d12.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/d3d12/context.h"
#include "common/d3d12/descriptor_heap_manager.h"
#include "common/d3d12/shader_cache.h"
#include "common/d3d12/util.h"
#include "common/log.h"
#include "common/scope_guard.h"
#include "common/timer.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "host_interface.h"
#include "system.h"
Log_SetChannel(GPU_HW_D3D12);

GPU_HW_D3D12::GPU_HW_D3D12() = default;

GPU_HW_D3D12::~GPU_HW_D3D12()
{
  if (m_host_display)
  {
    m_host_display->ClearDisplayTexture();
    ResetGraphicsAPIState();
  }

  DestroyResources();
}

GPURenderer GPU_HW_D3D12::GetRendererType() const
{
  return GPURenderer::HardwareD3D12;
}

bool GPU_HW_D3D12::Initialize(HostDisplay* host_display)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::D3D12)
  {
    Log_ErrorPrintf("Host render API is incompatible");
    return false;
  }

  SetCapabilities();

  if (!GPU_HW::Initialize(host_display))
    return false;

  if (!CreateRootSignatures())
  {
    Log_ErrorPrintf("Failed to create root signatures");
    return false;
  }

  if (!CreateSamplers())
  {
    Log_ErrorPrintf("Failed to create samplers");
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

  if (!CreateFramebuffer())
  {
    Log_ErrorPrintf("Failed to create framebuffer");
    return false;
  }

  if (!CompilePipelines())
  {
    Log_ErrorPrintf("Failed to compile pipelines");
    return false;
  }

  RestoreGraphicsAPIState();
  UpdateDepthBufferFromMaskBit();
  return true;
}

void GPU_HW_D3D12::Reset(bool clear_vram)
{
  GPU_HW::Reset(clear_vram);

  if (clear_vram)
    ClearFramebuffer();
}

void GPU_HW_D3D12::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();
}

void GPU_HW_D3D12::RestoreGraphicsAPIState()
{
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmdlist->OMSetRenderTargets(1, &m_vram_texture.GetRTVOrDSVDescriptor().cpu_handle, FALSE,
                              &m_vram_depth_texture.GetRTVOrDSVDescriptor().cpu_handle);

  const D3D12_VERTEX_BUFFER_VIEW vbv{m_vertex_stream_buffer.GetGPUPointer(), m_vertex_stream_buffer.GetSize(),
                                     sizeof(BatchVertex)};
  cmdlist->IASetVertexBuffers(0, 1, &vbv);
  cmdlist->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cmdlist->SetGraphicsRootSignature(m_batch_root_signature.Get());
  cmdlist->SetGraphicsRootConstantBufferView(0,
                                             m_uniform_stream_buffer.GetGPUPointer() + m_current_uniform_buffer_offset);
  cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_read_texture.GetSRVDescriptor().gpu_handle);
  cmdlist->SetGraphicsRootDescriptorTable(2, m_point_sampler.gpu_handle);

  D3D12::SetViewport(cmdlist, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());

  SetScissorFromDrawingArea();
}

void GPU_HW_D3D12::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  bool framebuffer_changed, shaders_changed;
  UpdateHWSettings(&framebuffer_changed, &shaders_changed);

  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    ResetGraphicsAPIState();
  }

  // Everything should be finished executing before recreating resources.
  m_host_display->ClearDisplayTexture();
  g_d3d12_context->ExecuteCommandList(true);

  if (framebuffer_changed)
    CreateFramebuffer();

  if (shaders_changed)
  {
    // clear it since we draw a loading screen and it's not in the correct state
    DestroyPipelines();
    CompilePipelines();
  }

  // this has to be done here, because otherwise we're using destroyed pipelines in the same cmdbuffer
  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_ptr, false, false);
    UpdateDepthBufferFromMaskBit();
    UpdateDisplay();
    ResetGraphicsAPIState();
  }
}

void GPU_HW_D3D12::MapBatchVertexPointer(u32 required_vertices)
{
  DebugAssert(!m_batch_start_vertex_ptr);

  const u32 required_space = required_vertices * sizeof(BatchVertex);
  if (!m_vertex_stream_buffer.ReserveMemory(required_space, sizeof(BatchVertex)))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in vertex stream buffer", required_space);
    g_d3d12_context->ExecuteCommandList(false);
    RestoreGraphicsAPIState();
    if (!m_vertex_stream_buffer.ReserveMemory(required_space, sizeof(BatchVertex)))
      Panic("Failed to reserve vertex stream buffer memory");
  }

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(m_vertex_stream_buffer.GetCurrentHostPointer());
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + (m_vertex_stream_buffer.GetCurrentSpace() / sizeof(BatchVertex));
  m_batch_base_vertex = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(BatchVertex);
}

void GPU_HW_D3D12::UnmapBatchVertexPointer(u32 used_vertices)
{
  DebugAssert(m_batch_start_vertex_ptr);
  if (used_vertices > 0)
    m_vertex_stream_buffer.CommitMemory(used_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;
}

void GPU_HW_D3D12::UploadUniformBuffer(const void* data, u32 data_size)
{
  if (!m_uniform_stream_buffer.ReserveMemory(data_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in uniform stream buffer", data_size);
    g_d3d12_context->ExecuteCommandList(false);
    RestoreGraphicsAPIState();
    if (!m_uniform_stream_buffer.ReserveMemory(data_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
      Panic("Failed to reserve uniform stream buffer memory");
  }

  m_current_uniform_buffer_offset = m_uniform_stream_buffer.GetCurrentOffset();
  std::memcpy(m_uniform_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_uniform_stream_buffer.CommitMemory(data_size);

  g_d3d12_context->GetCommandList()->SetGraphicsRootConstantBufferView(0, m_uniform_stream_buffer.GetGPUPointer() +
                                                                            m_current_uniform_buffer_offset);
}

void GPU_HW_D3D12::SetCapabilities()
{
  // TODO: Query from device
  const u32 max_texture_size = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  const u32 max_texture_scale = max_texture_size / VRAM_WIDTH;
  Log_InfoPrintf("Max texture size: %ux%u", max_texture_size, max_texture_size);
  m_max_resolution_scale = max_texture_scale;

  m_max_multisamples = 1;
  for (u32 multisamples = 2; multisamples < D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; multisamples++)
  {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS fd = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(multisamples)};

    if (SUCCEEDED(g_d3d12_context->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &fd,
                                                                    sizeof(fd))) &&
        fd.NumQualityLevels > 0)
    {
      m_max_multisamples = multisamples;
    }
  }

  m_supports_dual_source_blend = true;
  m_supports_per_sample_shading = true;
  Log_InfoPrintf("Dual-source blend: %s", m_supports_dual_source_blend ? "supported" : "not supported");
  Log_InfoPrintf("Per-sample shading: %s", m_supports_per_sample_shading ? "supported" : "not supported");
  Log_InfoPrintf("Max multisamples: %u", m_max_multisamples);
}

void GPU_HW_D3D12::DestroyResources()
{
  // Everything should be finished executing before recreating resources.
  if (g_d3d12_context)
    g_d3d12_context->ExecuteCommandList(true);

  DestroyFramebuffer();
  DestroyPipelines();

  g_d3d12_context->GetSamplerHeapManager().Free(&m_point_sampler);
  g_d3d12_context->GetSamplerHeapManager().Free(&m_linear_sampler);
  g_d3d12_context->GetDescriptorHeapManager().Free(&m_texture_stream_buffer_srv);

  m_vertex_stream_buffer.Destroy(false);
  m_uniform_stream_buffer.Destroy(false);
  m_texture_stream_buffer.Destroy(false);

  m_single_sampler_root_signature.Reset();
  m_batch_root_signature.Reset();
}

bool GPU_HW_D3D12::CreateRootSignatures()
{
  D3D12::RootSignatureBuilder rsbuilder;
  rsbuilder.SetInputAssemblerFlag();
  rsbuilder.AddCBVParameter(0, D3D12_SHADER_VISIBILITY_ALL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
  m_batch_root_signature = rsbuilder.Create();
  if (!m_batch_root_signature)
    return false;

  rsbuilder.Add32BitConstants(0, MAX_PUSH_CONSTANTS_SIZE / sizeof(u32), D3D12_SHADER_VISIBILITY_ALL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
  m_single_sampler_root_signature = rsbuilder.Create();
  if (!m_single_sampler_root_signature)
    return false;

  return true;
}

bool GPU_HW_D3D12::CreateSamplers()
{
  D3D12_SAMPLER_DESC desc = {};
  D3D12::SetDefaultSampler(&desc);
  desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;

  if (!g_d3d12_context->GetSamplerHeapManager().Allocate(&m_point_sampler))
    return false;

  g_d3d12_context->GetDevice()->CreateSampler(&desc, m_point_sampler.cpu_handle);

  desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;

  if (!g_d3d12_context->GetSamplerHeapManager().Allocate(&m_linear_sampler))
    return false;

  g_d3d12_context->GetDevice()->CreateSampler(&desc, m_linear_sampler.cpu_handle);
  return true;
}

bool GPU_HW_D3D12::CreateFramebuffer()
{
  DestroyFramebuffer();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const DXGI_FORMAT texture_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const DXGI_FORMAT depth_format = DXGI_FORMAT_D16_UNORM;

  if (!m_vram_texture.Create(texture_width, texture_height, m_multisamples, texture_format, texture_format,
                             texture_format, DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
      !m_vram_depth_texture.Create(
        texture_width, texture_height, m_multisamples, depth_format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
        depth_format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
      !m_vram_read_texture.Create(texture_width, texture_height, 1, texture_format, texture_format, DXGI_FORMAT_UNKNOWN,
                                  DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE) ||
      !m_display_texture.Create(texture_width, texture_height, 1, texture_format, texture_format, texture_format,
                                DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
      !m_vram_readback_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, texture_format, texture_format, texture_format,
                                      DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
      !m_vram_readback_staging_texture.Create(VRAM_WIDTH / 2, VRAM_HEIGHT, texture_format, false))
  {
    return false;
  }

  D3D12::SetObjectName(m_vram_texture, "VRAM Texture");
  D3D12::SetObjectName(m_vram_depth_texture, "VRAM Depth Texture");
  D3D12::SetObjectName(m_vram_read_texture, "VRAM Read/Sample Texture");
  D3D12::SetObjectName(m_display_texture, "VRAM Display Texture");
  D3D12::SetObjectName(m_vram_read_texture, "VRAM Readback Texture");

  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_vram_depth_texture.TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
  m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  ClearDisplay();
  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW_D3D12::ClearFramebuffer()
{
  static constexpr float clear_color[4] = {};

  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  cmdlist->ClearRenderTargetView(m_vram_texture.GetRTVOrDSVDescriptor(), clear_color, 0, nullptr);
  cmdlist->ClearDepthStencilView(m_vram_depth_texture.GetRTVOrDSVDescriptor(), D3D12_CLEAR_FLAG_DEPTH,
                                 m_pgxp_depth_buffer ? 1.0f : 0.0f, 0, 0, nullptr);
  SetFullVRAMDirtyRectangle();
}

void GPU_HW_D3D12::DestroyFramebuffer()
{
  m_vram_read_texture.Destroy(false);
  m_vram_depth_texture.Destroy(false);
  m_vram_texture.Destroy(false);
  m_vram_readback_texture.Destroy(false);
  m_display_texture.Destroy(false);
  m_vram_readback_staging_texture.Destroy(false);
}

bool GPU_HW_D3D12::CreateVertexBuffer()
{
  if (!m_vertex_stream_buffer.Create(VERTEX_BUFFER_SIZE))
    return false;

  D3D12::SetObjectName(m_vertex_stream_buffer.GetBuffer(), "Vertex Stream Buffer");
  return true;
}

bool GPU_HW_D3D12::CreateUniformBuffer()
{
  if (!m_uniform_stream_buffer.Create(UNIFORM_BUFFER_SIZE))
    return false;

  D3D12::SetObjectName(m_vertex_stream_buffer.GetBuffer(), "Uniform Stream Buffer");
  return true;
}

bool GPU_HW_D3D12::CreateTextureBuffer()
{
  if (!m_texture_stream_buffer.Create(VRAM_UPDATE_TEXTURE_BUFFER_SIZE))
    return false;

  if (!g_d3d12_context->GetDescriptorHeapManager().Allocate(&m_texture_stream_buffer_srv))
    return false;

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Format = DXGI_FORMAT_R16_UINT;
  desc.Buffer.NumElements = VRAM_UPDATE_TEXTURE_BUFFER_SIZE / sizeof(u16);
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  g_d3d12_context->GetDevice()->CreateShaderResourceView(m_texture_stream_buffer.GetBuffer(), &desc,
                                                         m_texture_stream_buffer_srv);

  D3D12::SetObjectName(m_texture_stream_buffer.GetBuffer(), "Texture Stream Buffer");
  return true;
}

bool GPU_HW_D3D12::CompilePipelines()
{
  D3D12::ShaderCache shader_cache;
  shader_cache.Open(g_host_interface->GetShaderCacheBasePath(), g_d3d12_context->GetFeatureLevel(),
                    g_settings.gpu_use_debug_device);

  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading,
                             m_true_color, m_scaled_dithering, m_texture_filtering, m_using_uv_limits,
                             m_pgxp_depth_buffer, m_supports_dual_source_blend);

  ShaderCompileProgressTracker progress("Compiling Pipelines", 2 + (4 * 9 * 2 * 2) + (2 * 4 * 5 * 9 * 2 * 2) + 1 +
                                                                 (2 * 2) + 2 + 2 + 1 + 1 + (2 * 3) + 1);

  // vertex shaders - [textured]
  // fragment shaders - [render_mode][texture_mode][dithering][interlacing]
  DimensionalArray<ComPtr<ID3DBlob>, 2> batch_vertex_shaders{};
  DimensionalArray<ComPtr<ID3DBlob>, 2, 2, 9, 4> batch_fragment_shaders{};

  for (u8 textured = 0; textured < 2; textured++)
  {
    const std::string vs = shadergen.GenerateBatchVertexShader(ConvertToBoolUnchecked(textured));
    batch_vertex_shaders[textured] = shader_cache.GetVertexShader(vs);
    if (!batch_vertex_shaders[textured])
      return false;

    progress.Increment();
  }

  for (u8 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        for (u8 interlacing = 0; interlacing < 2; interlacing++)
        {
          const std::string fs = shadergen.GenerateBatchFragmentShader(
            static_cast<BatchRenderMode>(render_mode), static_cast<GPUTextureMode>(texture_mode),
            ConvertToBoolUnchecked(dithering), ConvertToBoolUnchecked(interlacing));

          batch_fragment_shaders[render_mode][texture_mode][dithering][interlacing] = shader_cache.GetPixelShader(fs);
          if (!batch_fragment_shaders[render_mode][texture_mode][dithering][interlacing])
            return false;

          progress.Increment();
        }
      }
    }
  }

  D3D12::GraphicsPipelineBuilder gpbuilder;

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    for (u8 render_mode = 0; render_mode < 4; render_mode++)
    {
      for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
      {
        for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
        {
          for (u8 dithering = 0; dithering < 2; dithering++)
          {
            for (u8 interlacing = 0; interlacing < 2; interlacing++)
            {
              const bool textured = (static_cast<GPUTextureMode>(texture_mode) != GPUTextureMode::Disabled);

              gpbuilder.SetRootSignature(m_batch_root_signature.Get());
              gpbuilder.SetRenderTarget(0, m_vram_texture.GetFormat());
              gpbuilder.SetDepthStencilFormat(m_vram_depth_texture.GetFormat());

              gpbuilder.AddVertexAttribute("ATTR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(BatchVertex, x));
              gpbuilder.AddVertexAttribute("ATTR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(BatchVertex, color));
              if (textured)
              {
                gpbuilder.AddVertexAttribute("ATTR", 2, DXGI_FORMAT_R32_UINT, 0, offsetof(BatchVertex, u));
                gpbuilder.AddVertexAttribute("ATTR", 3, DXGI_FORMAT_R32_UINT, 0, offsetof(BatchVertex, texpage));
                if (m_using_uv_limits)
                  gpbuilder.AddVertexAttribute("ATTR", 4, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                               offsetof(BatchVertex, uv_limits));
              }

              gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
              gpbuilder.SetVertexShader(batch_vertex_shaders[BoolToUInt8(textured)].Get());
              gpbuilder.SetPixelShader(batch_fragment_shaders[render_mode][texture_mode][dithering][interlacing].Get());

              gpbuilder.SetRasterizationState(D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, false);
              gpbuilder.SetDepthState(true, true,
                                      (depth_test != 0) ? (m_pgxp_depth_buffer ? D3D12_COMPARISON_FUNC_LESS_EQUAL :
                                                                                 D3D12_COMPARISON_FUNC_GREATER_EQUAL) :
                                                          D3D12_COMPARISON_FUNC_ALWAYS);
              gpbuilder.SetNoBlendingState();
              gpbuilder.SetMultisamples(m_multisamples);

              if ((static_cast<GPUTransparencyMode>(transparency_mode) != GPUTransparencyMode::Disabled &&
                   (static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                    static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque)) ||
                  m_texture_filtering != GPUTextureFilter::Nearest)
              {
                gpbuilder.SetBlendState(
                  0, true, D3D12_BLEND_ONE,
                  m_supports_dual_source_blend ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC_ALPHA,
                  (static_cast<GPUTransparencyMode>(transparency_mode) ==
                     GPUTransparencyMode::BackgroundMinusForeground &&
                   static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                   static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                    D3D12_BLEND_OP_REV_SUBTRACT :
                    D3D12_BLEND_OP_ADD,
                  D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD);
              }

              m_batch_pipelines[depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing] =
                gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache);
              if (!m_batch_pipelines[depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing])
                return false;

              D3D12::SetObjectNameFormatted(
                m_batch_pipelines[depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
                  .Get(),
                "Batch Pipeline %u,%u,%u,%u,%u,%u", depth_test, render_mode, texture_mode, transparency_mode, dithering,
                interlacing);

              progress.Increment();
            }
          }
        }
      }
    }
  }

  ComPtr<ID3DBlob> fullscreen_quad_vertex_shader =
    shader_cache.GetVertexShader(shadergen.GenerateScreenQuadVertexShader());
  if (!fullscreen_quad_vertex_shader)
    return false;

  progress.Increment();

  // common state
  gpbuilder.SetRootSignature(m_single_sampler_root_signature.Get());
  gpbuilder.SetRenderTarget(0, m_vram_texture.GetFormat());
  gpbuilder.SetDepthStencilFormat(m_vram_depth_texture.GetFormat());
  gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
  gpbuilder.SetNoCullRasterizationState();
  gpbuilder.SetNoDepthTestState();
  gpbuilder.SetNoBlendingState();
  gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader.Get());
  gpbuilder.SetMultisamples(m_multisamples);
  gpbuilder.SetRenderTarget(0, m_vram_texture.GetFormat());
  gpbuilder.SetDepthStencilFormat(m_vram_depth_texture.GetFormat());

  // VRAM fill
  for (u8 wrapped = 0; wrapped < 2; wrapped++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(
        shadergen.GenerateVRAMFillFragmentShader(ConvertToBoolUnchecked(wrapped), ConvertToBoolUnchecked(interlaced)));
      if (!fs)
        return false;

      gpbuilder.SetPixelShader(fs.Get());
      gpbuilder.SetDepthState(true, true, D3D12_COMPARISON_FUNC_ALWAYS);

      m_vram_fill_pipelines[wrapped][interlaced] = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
      if (!m_vram_fill_pipelines[wrapped][interlaced])
        return false;

      D3D12::SetObjectNameFormatted(m_vram_fill_pipelines[wrapped][interlaced].Get(),
                                    "VRAM Fill Pipeline Wrapped=%u,Interlacing=%u", wrapped, interlaced);

      progress.Increment();
    }
  }

  // VRAM copy
  {
    ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateVRAMCopyFragmentShader());
    if (!fs)
      return false;

    gpbuilder.SetPixelShader(fs.Get());
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      gpbuilder.SetDepthState((depth_test != 0), true,
                              (depth_test != 0) ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS);

      m_vram_copy_pipelines[depth_test] = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
      if (!m_vram_copy_pipelines[depth_test])
        return false;

      D3D12::SetObjectNameFormatted(m_vram_copy_pipelines[depth_test].Get(), "VRAM Copy Pipeline Depth=%u", depth_test);

      progress.Increment();
    }
  }

  // VRAM write
  {
    ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateVRAMWriteFragmentShader(false));
    if (!fs)
      return false;

    gpbuilder.SetPixelShader(fs.Get());
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      gpbuilder.SetDepthState(true, true,
                              (depth_test != 0) ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS);
      m_vram_write_pipelines[depth_test] = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
      if (!m_vram_write_pipelines[depth_test])
        return false;

      D3D12::SetObjectNameFormatted(m_vram_write_pipelines[depth_test].Get(), "VRAM Write Pipeline Depth=%u",
                                    depth_test);

      progress.Increment();
    }
  }

  // VRAM update depth
  {
    ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateVRAMUpdateDepthFragmentShader());
    if (!fs)
      return false;

    gpbuilder.SetRootSignature(m_batch_root_signature.Get());
    gpbuilder.SetPixelShader(fs.Get());
    gpbuilder.SetDepthState(true, true, D3D12_COMPARISON_FUNC_ALWAYS);
    gpbuilder.SetBlendState(0, false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                            D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, 0);
    gpbuilder.ClearRenderTargets();

    m_vram_update_depth_pipeline = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
    if (!m_vram_update_depth_pipeline)
      return false;

    D3D12::SetObjectName(m_vram_update_depth_pipeline.Get(), "VRAM Update Depth Pipeline");

    progress.Increment();
  }

  gpbuilder.Clear();

  // VRAM read
  {
    ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateVRAMReadFragmentShader());
    if (!fs)
      return false;

    gpbuilder.SetRootSignature(m_single_sampler_root_signature.Get());
    gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader.Get());
    gpbuilder.SetPixelShader(fs.Get());
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetRenderTarget(0, m_vram_readback_texture.GetFormat());
    gpbuilder.ClearDepthStencilFormat();

    m_vram_readback_pipeline = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
    if (!m_vram_readback_pipeline)
      return false;

    D3D12::SetObjectName(m_vram_update_depth_pipeline.Get(), "VRAM Readback Pipeline");

    progress.Increment();
  }

  gpbuilder.Clear();

  // Display
  {
    gpbuilder.SetRootSignature(m_single_sampler_root_signature.Get());
    gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader.Get());
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetRenderTarget(0, m_display_texture.GetFormat());

    for (u8 depth_24 = 0; depth_24 < 2; depth_24++)
    {
      for (u8 interlace_mode = 0; interlace_mode < 3; interlace_mode++)
      {
        ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateDisplayFragmentShader(
          ConvertToBoolUnchecked(depth_24), static_cast<InterlacedRenderMode>(interlace_mode), m_chroma_smoothing));
        if (!fs)
          return false;

        gpbuilder.SetPixelShader(fs.Get());

        m_display_pipelines[depth_24][interlace_mode] =
          gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache, false);
        if (!m_display_pipelines[depth_24][interlace_mode])
          return false;

        D3D12::SetObjectNameFormatted(m_display_pipelines[depth_24][interlace_mode].Get(),
                                      "Display Pipeline Depth=%u Interlace=%u", depth_24, interlace_mode);

        progress.Increment();
      }
    }
  }

  // copy/blit
  {
    gpbuilder.Clear();
    gpbuilder.SetRootSignature(m_single_sampler_root_signature.Get());
    gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader.Get());
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);

    ComPtr<ID3DBlob> fs = shader_cache.GetPixelShader(shadergen.GenerateCopyFragmentShader());
    if (!fs)
      return false;

    gpbuilder.SetPixelShader(fs.Get());

    m_copy_pipeline = gpbuilder.Create(g_d3d12_context->GetDevice(), shader_cache);
    if (!m_copy_pipeline)
      return false;

    progress.Increment();
  }

#undef UPDATE_PROGRESS

  return true;
}

void GPU_HW_D3D12::DestroyPipelines()
{
  m_batch_pipelines = {};
  m_vram_fill_pipelines = {};
  m_vram_write_pipelines = {};
  m_vram_copy_pipelines = {};
  m_vram_readback_pipeline.Reset();
  m_vram_update_depth_pipeline.Reset();

  m_display_pipelines = {};
}

bool GPU_HW_D3D12::CreateTextureReplacementStreamBuffer()
{
  if (m_texture_replacment_stream_buffer.IsValid())
    return true;

  if (!m_texture_replacment_stream_buffer.Create(TEXTURE_REPLACEMENT_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate texture replacement streaming buffer");
    return false;
  }

  return true;
}

bool GPU_HW_D3D12::BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, u32 dst_x, u32 dst_y, u32 width,
                                              u32 height)
{
  if (!CreateTextureReplacementStreamBuffer())
    return false;

  if (m_vram_write_replacement_texture.GetWidth() < tex->GetWidth() ||
      m_vram_write_replacement_texture.GetHeight() < tex->GetHeight())
  {
    if (!m_vram_write_replacement_texture.Create(tex->GetWidth(), tex->GetHeight(), 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                                 D3D12_RESOURCE_FLAG_NONE))
    {
      Log_ErrorPrint("Failed to create VRAM write replacement texture");
      return false;
    }
  }

  const u32 copy_pitch = Common::AlignUpPow2<u32>(tex->GetWidth() * sizeof(u32), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 required_size = copy_pitch * tex->GetHeight();
  if (!m_texture_replacment_stream_buffer.ReserveMemory(required_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
  {
    Log_PerfPrint("Executing command buffer while waiting for texture replacement buffer space");
    g_d3d12_context->ExecuteCommandList(false);
    RestoreGraphicsAPIState();
    if (!m_texture_replacment_stream_buffer.ReserveMemory(required_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
    {
      Log_ErrorPrintf("Failed to allocate %u bytes from texture replacement streaming buffer", required_size);
      return false;
    }
  }

  // buffer -> texture
  const u32 sb_offset = m_texture_replacment_stream_buffer.GetCurrentOffset();
  D3D12::Texture::CopyToUploadBuffer(tex->GetPixels(), tex->GetByteStride(), tex->GetHeight(),
                                     m_texture_replacment_stream_buffer.GetCurrentHostPointer(), copy_pitch);
  m_texture_replacment_stream_buffer.CommitMemory(sb_offset);
  m_vram_write_replacement_texture.CopyFromBuffer(0, 0, tex->GetWidth(), tex->GetHeight(), copy_pitch,
                                                  m_texture_replacment_stream_buffer.GetBuffer(), sb_offset);
  m_vram_write_replacement_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // texture -> vram
  const float uniforms[] = {
    0.0f, 0.0f, static_cast<float>(tex->GetWidth()) / static_cast<float>(m_vram_write_replacement_texture.GetWidth()),
    static_cast<float>(tex->GetHeight()) / static_cast<float>(m_vram_write_replacement_texture.GetHeight())};
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
  cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), uniforms, 0);
  cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_write_replacement_texture.GetSRVDescriptor());
  cmdlist->SetGraphicsRootDescriptorTable(2, m_linear_sampler.gpu_handle);
  cmdlist->SetPipelineState(m_copy_pipeline.Get());
  D3D12::SetViewportAndScissor(cmdlist, dst_x, dst_y, width, height);
  cmdlist->DrawInstanced(3, 1, 0, 0);
  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_D3D12::DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices)
{
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();

  // [primitive][depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  ID3D12PipelineState* pipeline =
    m_batch_pipelines[BoolToUInt8(m_batch.check_mask_before_draw || m_batch.use_depth_buffer)][static_cast<u8>(
      render_mode)][static_cast<u8>(m_batch.texture_mode)][static_cast<u8>(m_batch.transparency_mode)]
                     [BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)]
                       .Get();

  cmdlist->SetPipelineState(pipeline);
  cmdlist->DrawInstanced(num_vertices, 1, base_vertex, 0);
}

void GPU_HW_D3D12::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  D3D12::SetScissor(g_d3d12_context->GetCommandList(), left, top, right - left, bottom - top);
}

void GPU_HW_D3D12::ClearDisplay()
{
  GPU_HW::ClearDisplay();

  m_host_display->ClearDisplayTexture();

  static constexpr float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  m_display_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  g_d3d12_context->GetCommandList()->ClearRenderTargetView(m_display_texture.GetRTVOrDSVDescriptor(), clear_color, 0,
                                                           nullptr);
}

void GPU_HW_D3D12::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (g_settings.debugging.show_vram)
  {
    if (IsUsingMultisampling())
    {
      UpdateVRAMReadTexture();
      m_host_display->SetDisplayTexture(&m_vram_read_texture, HostDisplayPixelFormat::RGBA8,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight(), 0, 0,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight());
    }
    else
    {
      m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), 0, 0, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight());
    }
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
  else
  {
    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());

    const u32 resolution_scale = m_GPUSTAT.display_area_color_depth_24 ? 1 : m_resolution_scale;
    const u32 vram_offset_x = m_crtc_state.display_vram_left;
    const u32 vram_offset_y = m_crtc_state.display_vram_top;
    const u32 scaled_vram_offset_x = vram_offset_x * resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * resolution_scale;
    const u32 display_width = m_crtc_state.display_vram_width;
    const u32 display_height = m_crtc_state.display_vram_height;
    const u32 scaled_display_width = display_width * resolution_scale;
    const u32 scaled_display_height = display_height * resolution_scale;
    const InterlacedRenderMode interlaced = GetInterlacedRenderMode();

    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && interlaced == InterlacedRenderMode::None &&
             !IsUsingMultisampling() && (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture.GetWidth() &&
             (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture.GetHeight())
    {
      m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), scaled_vram_offset_x, scaled_vram_offset_y,
                                        scaled_display_width, scaled_display_height);
    }
    else
    {
      const u32 reinterpret_field_offset = (interlaced != InterlacedRenderMode::None) ? GetInterlacedDisplayField() : 0;
      const u32 reinterpret_start_x = m_crtc_state.regs.X * resolution_scale;
      const u32 reinterpret_crop_left = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * resolution_scale;
      const u32 uniforms[4] = {reinterpret_start_x, scaled_vram_offset_y + reinterpret_field_offset,
                               reinterpret_crop_left, reinterpret_field_offset};

      ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
      m_display_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
      m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

      cmdlist->OMSetRenderTargets(1, &m_display_texture.GetRTVOrDSVDescriptor().cpu_handle, FALSE, nullptr);
      cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
      cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), uniforms, 0);
      cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_texture.GetSRVDescriptor());
      cmdlist->SetPipelineState(
        m_display_pipelines[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)][static_cast<u8>(interlaced)].Get());
      D3D12::SetViewportAndScissor(cmdlist, 0, 0, scaled_display_width, scaled_display_height);
      cmdlist->DrawInstanced(3, 1, 0, 0);

      m_display_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

      m_host_display->SetDisplayTexture(&m_display_texture, HostDisplayPixelFormat::RGBA8, m_display_texture.GetWidth(),
                                        m_display_texture.GetHeight(), 0, 0, scaled_display_width,
                                        scaled_display_height);

      RestoreGraphicsAPIState();
    }
  }
}

void GPU_HW_D3D12::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  if (IsUsingSoftwareRendererForReadbacks())
  {
    ReadSoftwareRendererVRAM(x, y, width, height);
    return;
  }

  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_vram_readback_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Encode the 24-bit texture as 16-bit.
  const u32 uniforms[4] = {copy_rect.left, copy_rect.top, copy_rect.GetWidth(), copy_rect.GetHeight()};
  cmdlist->OMSetRenderTargets(1, &m_vram_readback_texture.GetRTVOrDSVDescriptor().cpu_handle, FALSE, nullptr);
  cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
  cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), uniforms, 0);
  cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_texture.GetSRVDescriptor());
  cmdlist->SetPipelineState(m_vram_readback_pipeline.Get());
  D3D12::SetViewportAndScissor(cmdlist, 0, 0, encoded_width, encoded_height);
  cmdlist->DrawInstanced(3, 1, 0, 0);

  m_vram_readback_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  // Stage the readback.
  m_vram_readback_staging_texture.CopyFromTexture(m_vram_readback_texture, 0, 0, 0, 0, 0, encoded_width,
                                                  encoded_height);

  // And copy it into our shadow buffer (will execute command buffer and stall).
  m_vram_readback_staging_texture.ReadPixels(0, 0, encoded_width, encoded_height,
                                             &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left],
                                             VRAM_WIDTH * sizeof(u16));

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D12::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  if (IsUsingSoftwareRendererForReadbacks())
    FillSoftwareRendererVRAM(x, y, width, height, color);

  // TODO: Use fast clear
  GPU_HW::FillVRAM(x, y, width, height, color);

  const VRAMFillUBOData uniforms = GetVRAMFillUBOData(x, y, width, height, color);
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();

  const bool wrapped = IsVRAMFillOversized(x, y, width, height);
  const bool interlaced = IsInterlacedRenderingEnabled();
  if (!wrapped && !interlaced)
  {
    const D3D12_RECT rc = {static_cast<LONG>(x * m_resolution_scale), static_cast<LONG>(y * m_resolution_scale),
                           static_cast<LONG>((x + width) * m_resolution_scale),
                           static_cast<LONG>((y + height) * m_resolution_scale)};
    cmdlist->ClearRenderTargetView(m_vram_texture.GetRTVOrDSVDescriptor(), uniforms.u_fill_color, 1, &rc);
    cmdlist->ClearDepthStencilView(m_vram_depth_texture.GetRTVOrDSVDescriptor(), D3D12_CLEAR_FLAG_DEPTH,
                                   uniforms.u_fill_color[3], 0, 1, &rc);
    return;
  }

  cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
  cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), &uniforms, 0);
  cmdlist->SetGraphicsRootDescriptorTable(1, g_d3d12_context->GetNullSRVDescriptor());
  cmdlist->SetPipelineState(m_vram_fill_pipelines[BoolToUInt8(IsVRAMFillOversized(x, y, width, height))]
                                                 [BoolToUInt8(IsInterlacedRenderingEnabled())]
                                                   .Get());

  const Common::Rectangle<u32> bounds(GetVRAMTransferBounds(x, y, width, height));
  D3D12::SetViewportAndScissor(cmdlist, bounds.left * m_resolution_scale, bounds.top * m_resolution_scale,
                               bounds.GetWidth() * m_resolution_scale, bounds.GetHeight() * m_resolution_scale);

  cmdlist->DrawInstanced(3, 1, 0, 0);

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D12::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  if (IsUsingSoftwareRendererForReadbacks())
    UpdateSoftwareRendererVRAM(x, y, width, height, data, set_mask, check_mask);

  const Common::Rectangle<u32> bounds = GetVRAMTransferBounds(x, y, width, height);
  GPU_HW::UpdateVRAM(bounds.left, bounds.top, bounds.GetWidth(), bounds.GetHeight(), data, set_mask, check_mask);

  if (!check_mask)
  {
    const TextureReplacementTexture* rtex = g_texture_replacements.GetVRAMWriteReplacement(width, height, data);
    if (rtex && BlitVRAMReplacementTexture(rtex, x * m_resolution_scale, y * m_resolution_scale,
                                           width * m_resolution_scale, height * m_resolution_scale))
    {
      return;
    }
  }

  const u32 data_size = width * height * sizeof(u16);
  const u32 alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // ???
  if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in stream buffer", data_size);
    g_d3d12_context->ExecuteCommandList(false);
    RestoreGraphicsAPIState();
    if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
    {
      Panic("Failed to allocate space in stream buffer for VRAM write");
      return;
    }
  }

  const u32 start_index = m_texture_stream_buffer.GetCurrentOffset() / sizeof(u16);
  std::memcpy(m_texture_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_texture_stream_buffer.CommitMemory(data_size);

  const VRAMWriteUBOData uniforms = GetVRAMWriteUBOData(x, y, width, height, start_index, set_mask, check_mask);

  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
  cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), &uniforms, 0);
  cmdlist->SetGraphicsRootDescriptorTable(1, m_texture_stream_buffer_srv);
  cmdlist->SetPipelineState(m_vram_write_pipelines[BoolToUInt8(check_mask)].Get());

  // the viewport should already be set to the full vram, so just adjust the scissor
  const Common::Rectangle<u32> scaled_bounds = bounds * m_resolution_scale;
  D3D12::SetScissor(cmdlist, scaled_bounds.left, scaled_bounds.top, scaled_bounds.GetWidth(),
                    scaled_bounds.GetHeight());

  cmdlist->DrawInstanced(3, 1, 0, 0);

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D12::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  if (IsUsingSoftwareRendererForReadbacks())
    CopySoftwareRendererVRAM(src_x, src_y, dst_x, dst_y, width, height);

  if (UseVRAMCopyShader(src_x, src_y, dst_x, dst_y, width, height) || IsUsingMultisampling())
  {
    const Common::Rectangle<u32> src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
    const Common::Rectangle<u32> dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
    if (m_vram_dirty_rect.Intersects(src_bounds))
      UpdateVRAMReadTexture();
    IncludeVRAMDirtyRectangle(dst_bounds);

    const VRAMCopyUBOData uniforms(GetVRAMCopyUBOData(src_x, src_y, dst_x, dst_y, width, height));
    const Common::Rectangle<u32> dst_bounds_scaled(dst_bounds * m_resolution_scale);

    ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
    cmdlist->SetGraphicsRootSignature(m_single_sampler_root_signature.Get());
    cmdlist->SetGraphicsRoot32BitConstants(0, sizeof(uniforms) / sizeof(u32), &uniforms, 0);
    cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_read_texture.GetSRVDescriptor());
    cmdlist->SetPipelineState(m_vram_copy_pipelines[BoolToUInt8(m_GPUSTAT.check_mask_before_draw)].Get());
    D3D12::SetViewportAndScissor(cmdlist, dst_bounds_scaled.left, dst_bounds_scaled.top, dst_bounds_scaled.GetWidth(),
                                 dst_bounds_scaled.GetHeight());
    cmdlist->DrawInstanced(3, 1, 0, 0);

    RestoreGraphicsAPIState();

    if (m_GPUSTAT.check_mask_before_draw)
      m_current_depth++;

    return;
  }

  if (m_vram_dirty_rect.Intersects(Common::Rectangle<u32>::FromExtents(src_x, src_y, width, height)))
    UpdateVRAMReadTexture();

  GPU_HW::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);

  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  const D3D12_TEXTURE_COPY_LOCATION src = {m_vram_read_texture.GetResource(),
                                           D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION dst = {m_vram_texture.GetResource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_BOX src_box = {src_x, src_y, 0u, src_x + width, src_y + height, 1u};

  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
  m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);

  g_d3d12_context->GetCommandList()->CopyTextureRegion(&dst, dst_x, dst_y, 0, &src, &src_box);

  m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void GPU_HW_D3D12::UpdateVRAMReadTexture()
{
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();

  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;

  if (m_vram_texture.IsMultisampled())
  {
    m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_RESOLVE_DEST);
    cmdlist->ResolveSubresource(m_vram_read_texture, 0, m_vram_texture, 0, m_vram_texture.GetFormat());
  }
  else
  {
    const D3D12_TEXTURE_COPY_LOCATION src = {m_vram_texture.GetResource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_TEXTURE_COPY_LOCATION dst = {m_vram_read_texture.GetResource(),
                                             D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_BOX src_box = {scaled_rect.left, scaled_rect.top, 0u, scaled_rect.right, scaled_rect.bottom, 1u};
    m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
    cmdlist->CopyTextureRegion(&dst, scaled_rect.left, scaled_rect.top, 0, &src, &src_box);
  }

  m_vram_read_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  GPU_HW::UpdateVRAMReadTexture();
}

void GPU_HW_D3D12::UpdateDepthBufferFromMaskBit()
{
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();

  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  cmdlist->OMSetRenderTargets(0, nullptr, FALSE, &m_vram_depth_texture.GetRTVOrDSVDescriptor().cpu_handle);
  cmdlist->SetGraphicsRootDescriptorTable(1, m_vram_texture.GetSRVDescriptor());
  cmdlist->SetPipelineState(m_vram_update_depth_pipeline.Get());
  D3D12::SetViewportAndScissor(cmdlist, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
  cmdlist->DrawInstanced(3, 1, 0, 0);

  m_vram_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  RestoreGraphicsAPIState();
}

void GPU_HW_D3D12::ClearDepthBuffer()
{
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  cmdlist->ClearDepthStencilView(m_vram_depth_texture.GetRTVOrDSVDescriptor(), D3D12_CLEAR_FLAG_DEPTH,
                                 m_pgxp_depth_buffer ? 1.0f : 0.0f, 0, 0, nullptr);
}

std::unique_ptr<GPU> GPU::CreateHardwareD3D12Renderer()
{
  return std::make_unique<GPU_HW_D3D12>();
}
