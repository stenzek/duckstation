// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_device.h"
#include "core/host.h"     // TODO: Remove, needed for getting fullscreen mode.
#include "core/settings.h" // TODO: Remove, needed for dump directory.
#include "gpu_framebuffer_manager.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "fmt/format.h"
#include "imgui.h"
#include "xxhash.h"

Log_SetChannel(GPUDevice);

#ifdef _WIN32
#include "common/windows_headers.h"
#include "d3d11_device.h"
#include "d3d12_device.h"
#include "d3d_common.h"
#endif

#ifdef ENABLE_OPENGL
#include "opengl_device.h"
#endif

#ifdef ENABLE_VULKAN
#include "vulkan_device.h"
#endif

std::unique_ptr<GPUDevice> g_gpu_device;

static std::string s_pipeline_cache_path;

GPUSampler::GPUSampler() = default;

GPUSampler::~GPUSampler() = default;

GPUSampler::Config GPUSampler::GetNearestConfig()
{
  Config config = {};
  config.address_u = GPUSampler::AddressMode::ClampToEdge;
  config.address_v = GPUSampler::AddressMode::ClampToEdge;
  config.address_w = GPUSampler::AddressMode::ClampToEdge;
  config.min_filter = GPUSampler::Filter::Nearest;
  config.mag_filter = GPUSampler::Filter::Nearest;
  return config;
}

GPUSampler::Config GPUSampler::GetLinearConfig()
{
  Config config = {};
  config.address_u = GPUSampler::AddressMode::ClampToEdge;
  config.address_v = GPUSampler::AddressMode::ClampToEdge;
  config.address_w = GPUSampler::AddressMode::ClampToEdge;
  config.min_filter = GPUSampler::Filter::Linear;
  config.mag_filter = GPUSampler::Filter::Linear;
  return config;
}

GPUShader::GPUShader(GPUShaderStage stage) : m_stage(stage)
{
}

GPUShader::~GPUShader() = default;

const char* GPUShader::GetStageName(GPUShaderStage stage)
{
  static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> names = {"Vertex", "Fragment",
                                                                                                "Geometry", "Compute"};

  return names[static_cast<u32>(stage)];
}

GPUPipeline::GPUPipeline() = default;

GPUPipeline::~GPUPipeline() = default;

size_t GPUPipeline::InputLayoutHash::operator()(const InputLayout& il) const
{
  std::size_t h = 0;
  hash_combine(h, il.vertex_attributes.size(), il.vertex_stride);

  for (const VertexAttribute& va : il.vertex_attributes)
    hash_combine(h, va.key);

  return h;
}

bool GPUPipeline::InputLayout::operator==(const InputLayout& rhs) const
{
  return (vertex_stride == rhs.vertex_stride && vertex_attributes.size() == rhs.vertex_attributes.size() &&
          std::memcmp(vertex_attributes.data(), rhs.vertex_attributes.data(),
                      sizeof(VertexAttribute) * rhs.vertex_attributes.size()) == 0);
}

bool GPUPipeline::InputLayout::operator!=(const InputLayout& rhs) const
{
  return (vertex_stride != rhs.vertex_stride || vertex_attributes.size() != rhs.vertex_attributes.size() ||
          std::memcmp(vertex_attributes.data(), rhs.vertex_attributes.data(),
                      sizeof(VertexAttribute) * rhs.vertex_attributes.size()) != 0);
}

GPUPipeline::RasterizationState GPUPipeline::RasterizationState::GetNoCullState()
{
  RasterizationState ret = {};
  ret.cull_mode = CullMode::None;
  return ret;
}

GPUPipeline::DepthState GPUPipeline::DepthState::GetNoTestsState()
{
  DepthState ret = {};
  ret.depth_test = DepthFunc::Always;
  return ret;
}

GPUPipeline::DepthState GPUPipeline::DepthState::GetAlwaysWriteState()
{
  DepthState ret = {};
  ret.depth_test = DepthFunc::Always;
  ret.depth_write = true;
  return ret;
}

GPUPipeline::BlendState GPUPipeline::BlendState::GetNoBlendingState()
{
  BlendState ret = {};
  ret.write_mask = 0xf;
  return ret;
}

GPUPipeline::BlendState GPUPipeline::BlendState::GetAlphaBlendingState()
{
  BlendState ret = {};
  ret.enable = true;
  ret.src_blend = BlendFunc::SrcAlpha;
  ret.dst_blend = BlendFunc::InvSrcAlpha;
  ret.blend_op = BlendOp::Add;
  ret.src_alpha_blend = BlendFunc::One;
  ret.dst_alpha_blend = BlendFunc::Zero;
  ret.alpha_blend_op = BlendOp::Add;
  ret.write_mask = 0xf;
  return ret;
}

void GPUPipeline::GraphicsConfig::SetTargetFormats(GPUTexture::Format color_format,
                                                   GPUTexture::Format depth_format_ /* = GPUTexture::Format::Unknown */)
{
  color_formats[0] = color_format;
  for (size_t i = 1; i < std::size(color_formats); i++)
    color_formats[i] = GPUTexture::Format::Unknown;
  depth_format = depth_format_;
}

GPUTextureBuffer::GPUTextureBuffer(Format format, u32 size) : m_format(format), m_size_in_elements(size)
{
}

GPUTextureBuffer::~GPUTextureBuffer() = default;

u32 GPUTextureBuffer::GetElementSize(Format format)
{
  static constexpr std::array<u32, static_cast<u32>(Format::MaxCount)> element_size = {{
    sizeof(u16),
  }};

  return element_size[static_cast<u32>(format)];
}

bool GPUFramebufferManagerBase::Key::operator==(const Key& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool GPUFramebufferManagerBase::Key::operator!=(const Key& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

bool GPUFramebufferManagerBase::Key::ContainsRT(const GPUTexture* tex) const
{
  // num_rts is worse for predictability.
  for (u32 i = 0; i < GPUDevice::MAX_RENDER_TARGETS; i++)
  {
    if (rts[i] == tex)
      return true;
  }
  return false;
}

size_t GPUFramebufferManagerBase::KeyHash::operator()(const Key& key) const
{
  if constexpr (sizeof(void*) == 8)
    return XXH3_64bits(&key, sizeof(key));
  else
    return XXH32(&key, sizeof(key), 0x1337);
}

GPUDevice::~GPUDevice() = default;

RenderAPI GPUDevice::GetPreferredAPI()
{
#if defined(_WIN32) && !defined(_M_ARM64)
  // Perfer DX11 on Windows, except ARM64, where QCom has slow DX11 drivers.
  return RenderAPI::D3D11;
#elif defined(_WIN32) && defined(_M_ARM64)
  return RenderAPI::D3D12;
#elif defined(__APPLE__)
  // Prefer Metal on MacOS.
  return RenderAPI::Metal;
#elif defined(ENABLE_OPENGL) && defined(ENABLE_VULKAN)
  // On Linux, if we have both GL and Vulkan, prefer VK if the driver isn't software.
  return VulkanDevice::IsSuitableDefaultRenderer() ? RenderAPI::Vulkan : RenderAPI::OpenGL;
#elif defined(ENABLE_OPENGL)
  return RenderAPI::OpenGL;
#elif defined(ENABLE_VULKAN)
  return RenderAPI::Vulkan;
#else
  // Uhhh, what?
  return RenderAPI::None;
#endif
}

const char* GPUDevice::RenderAPIToString(RenderAPI api)
{
  switch (api)
  {
    // clang-format off
#define CASE(x) case RenderAPI::x: return #x
    CASE(None);
    CASE(D3D11);
    CASE(D3D12);
    CASE(Metal);
    CASE(Vulkan);
    CASE(OpenGL);
    CASE(OpenGLES);
#undef CASE
      // clang-format on
    default:
      return "Unknown";
  }
}

bool GPUDevice::IsSameRenderAPI(RenderAPI lhs, RenderAPI rhs)
{
  return (lhs == rhs || ((lhs == RenderAPI::OpenGL || lhs == RenderAPI::OpenGLES) &&
                         (rhs == RenderAPI::OpenGL || rhs == RenderAPI::OpenGLES)));
}

bool GPUDevice::Create(const std::string_view& adapter, const std::string_view& shader_cache_path,
                       u32 shader_cache_version, bool debug_device, bool vsync, bool threaded_presentation,
                       FeatureMask disabled_features)
{
  m_vsync_enabled = vsync;
  m_debug_device = debug_device;

  if (!AcquireWindow(true))
  {
    Log_ErrorPrintf("Failed to acquire window from host.");
    return false;
  }

  if (!CreateDevice(adapter, threaded_presentation, disabled_features))
  {
    Log_ErrorPrintf("Failed to create device.");
    return false;
  }

  Log_InfoPrintf("Graphics Driver Info:\n%s", GetDriverInfo().c_str());

  OpenShaderCache(shader_cache_path, shader_cache_version);

  if (!CreateResources())
  {
    Log_ErrorPrintf("Failed to create base resources.");
    return false;
  }

  return true;
}

void GPUDevice::Destroy()
{
  if (HasSurface())
    DestroySurface();
  DestroyResources();
  CloseShaderCache();
  DestroyDevice();
}

bool GPUDevice::SupportsExclusiveFullscreen() const
{
  return false;
}

void GPUDevice::OpenShaderCache(const std::string_view& base_path, u32 version)
{
  if (m_features.shader_cache && !base_path.empty())
  {
    const std::string basename = GetShaderCacheBaseName("shaders");
    const std::string filename = Path::Combine(base_path, basename);
    if (!m_shader_cache.Open(filename.c_str(), version))
    {
      Log_WarningPrintf("Failed to open shader cache. Creating new cache.");
      if (!m_shader_cache.Create())
        Log_ErrorPrintf("Failed to create new shader cache.");

      // Squish the pipeline cache too, it's going to be stale.
      if (m_features.pipeline_cache)
      {
        const std::string pc_filename =
          Path::Combine(base_path, TinyString::from_fmt("{}.bin", GetShaderCacheBaseName("pipelines")));
        if (FileSystem::FileExists(pc_filename.c_str()))
        {
          Log_InfoPrintf("Removing old pipeline cache '%s'", pc_filename.c_str());
          FileSystem::DeleteFile(pc_filename.c_str());
        }
      }
    }
  }
  else
  {
    // Still need to set the version - GL needs it.
    m_shader_cache.Open(std::string_view(), version);
  }

  s_pipeline_cache_path = {};
  if (m_features.pipeline_cache && !base_path.empty())
  {
    const std::string basename = GetShaderCacheBaseName("pipelines");
    const std::string filename = Path::Combine(base_path, TinyString::from_fmt("{}.bin", basename));
    if (ReadPipelineCache(filename))
      s_pipeline_cache_path = std::move(filename);
    else
      Log_WarningPrintf("Failed to read pipeline cache.");
  }
}

void GPUDevice::CloseShaderCache()
{
  m_shader_cache.Close();

  if (!s_pipeline_cache_path.empty())
  {
    DynamicHeapArray<u8> data;
    if (GetPipelineCacheData(&data))
    {
      // Save disk writes if it hasn't changed, think of the poor SSDs.
      FILESYSTEM_STAT_DATA sd;
      if (!FileSystem::StatFile(s_pipeline_cache_path.c_str(), &sd) || sd.Size != static_cast<s64>(data.size()))
      {
        Log_InfoPrintf("Writing %zu bytes to '%s'", data.size(), s_pipeline_cache_path.c_str());
        if (!FileSystem::WriteBinaryFile(s_pipeline_cache_path.c_str(), data.data(), data.size()))
          Log_ErrorPrintf("Failed to write pipeline cache to '%s'", s_pipeline_cache_path.c_str());
      }
      else
      {
        Log_InfoPrintf("Skipping updating pipeline cache '%s' due to no changes.", s_pipeline_cache_path.c_str());
      }
    }

    s_pipeline_cache_path = {};
  }
}

std::string GPUDevice::GetShaderCacheBaseName(const std::string_view& type) const
{
  const std::string_view debug_suffix = m_debug_device ? "_debug" : "";

  std::string ret;
  switch (GetRenderAPI())
  {
#ifdef _WIN32
    case RenderAPI::D3D11:
      ret = fmt::format(
        "d3d11_{}_{}{}", type,
        D3DCommon::GetFeatureLevelShaderModelString(D3D11Device::GetInstance().GetD3DDevice()->GetFeatureLevel()),
        debug_suffix);
      break;
    case RenderAPI::D3D12:
      ret = fmt::format("d3d12_{}{}", type, debug_suffix);
      break;
#endif
#ifdef ENABLE_VULKAN
    case RenderAPI::Vulkan:
      ret = fmt::format("vulkan_{}{}", type, debug_suffix);
      break;
#endif
#ifdef ENABLE_OPENGL
    case RenderAPI::OpenGL:
      ret = fmt::format("opengl_{}{}", type, debug_suffix);
      break;
    case RenderAPI::OpenGLES:
      ret = fmt::format("opengles_{}{}", type, debug_suffix);
      break;
#endif
#ifdef __APPLE__
    case RenderAPI::Metal:
      ret = fmt::format("metal_{}{}", type, debug_suffix);
      break;
#endif
    default:
      UnreachableCode();
      break;
  }

  return ret;
}

bool GPUDevice::ReadPipelineCache(const std::string& filename)
{
  return false;
}

bool GPUDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data)
{
  return false;
}

bool GPUDevice::AcquireWindow(bool recreate_window)
{
  std::optional<WindowInfo> wi = Host::AcquireRenderWindow(recreate_window);
  if (!wi.has_value())
    return false;

  Log_InfoPrintf("Render window is %ux%u.", wi->surface_width, wi->surface_height);
  m_window_info = wi.value();
  return true;
}

bool GPUDevice::CreateResources()
{
  if (!(m_nearest_sampler = CreateSampler(GPUSampler::GetNearestConfig())))
    return false;

  if (!(m_linear_sampler = CreateSampler(GPUSampler::GetLinearConfig())))
    return false;

  ShaderGen shadergen(GetRenderAPI(), m_features.dual_source_blend, m_features.framebuffer_fetch);

  std::unique_ptr<GPUShader> imgui_vs = CreateShader(GPUShaderStage::Vertex, shadergen.GenerateImGuiVertexShader());
  std::unique_ptr<GPUShader> imgui_fs = CreateShader(GPUShaderStage::Fragment, shadergen.GenerateImGuiFragmentShader());
  if (!imgui_vs || !imgui_fs)
    return false;
  GL_OBJECT_NAME(imgui_vs, "ImGui Vertex Shader");
  GL_OBJECT_NAME(imgui_fs, "ImGui Fragment Shader");

  static constexpr GPUPipeline::VertexAttribute imgui_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, offsetof(ImDrawVert, pos)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, offsetof(ImDrawVert, uv)),
    GPUPipeline::VertexAttribute::Make(2, GPUPipeline::VertexAttribute::Semantic::Color, 0,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, offsetof(ImDrawVert, col)),
  };

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.input_layout.vertex_attributes = imgui_attributes;
  plconfig.input_layout.vertex_stride = sizeof(ImDrawVert);
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetAlphaBlendingState();
  plconfig.blend.write_mask = 0x7;
  plconfig.SetTargetFormats(HasSurface() ? m_window_info.surface_format : GPUTexture::Format::RGBA8);
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.vertex_shader = imgui_vs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.fragment_shader = imgui_fs.get();

  m_imgui_pipeline = CreatePipeline(plconfig);
  if (!m_imgui_pipeline)
  {
    Log_ErrorPrintf("Failed to compile ImGui pipeline.");
    return false;
  }
  GL_OBJECT_NAME(m_imgui_pipeline, "ImGui Pipeline");

  return true;
}

void GPUDevice::DestroyResources()
{
  m_imgui_font_texture.reset();
  m_imgui_pipeline.reset();

  m_imgui_pipeline.reset();

  m_linear_sampler.reset();
  m_nearest_sampler.reset();

  m_shader_cache.Close();
}

void GPUDevice::RenderImGui()
{
  GL_SCOPE("RenderImGui");

  ImGui::Render();

  const ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data->CmdListsCount == 0)
    return;

  SetPipeline(m_imgui_pipeline.get());
  SetViewportAndScissor(0, 0, m_window_info.surface_width, m_window_info.surface_height);

  const float L = 0.0f;
  const float R = static_cast<float>(m_window_info.surface_width);
  const float T = 0.0f;
  const float B = static_cast<float>(m_window_info.surface_height);
  const float ortho_projection[4][4] = {
    {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
    {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
    {0.0f, 0.0f, 0.5f, 0.0f},
    {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f},
  };
  PushUniformBuffer(ortho_projection, sizeof(ortho_projection));

  // Render command lists
  for (int n = 0; n < draw_data->CmdListsCount; n++)
  {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];
    static_assert(sizeof(ImDrawIdx) == sizeof(DrawIndex));

    u32 base_vertex, base_index;
    UploadVertexBuffer(cmd_list->VtxBuffer.Data, sizeof(ImDrawVert), cmd_list->VtxBuffer.Size, &base_vertex);
    UploadIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size, &base_index);

    for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
    {
      const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
      DebugAssert(!pcmd->UserCallback);

      if (pcmd->ElemCount == 0 || pcmd->ClipRect.z <= pcmd->ClipRect.x || pcmd->ClipRect.w <= pcmd->ClipRect.y)
        continue;

      SetScissor(static_cast<s32>(pcmd->ClipRect.x), static_cast<s32>(pcmd->ClipRect.y),
                 static_cast<s32>(pcmd->ClipRect.z - pcmd->ClipRect.x),
                 static_cast<s32>(pcmd->ClipRect.w - pcmd->ClipRect.y));
      SetTextureSampler(0, reinterpret_cast<GPUTexture*>(pcmd->TextureId), m_linear_sampler.get());
      DrawIndexed(pcmd->ElemCount, base_index + pcmd->IdxOffset, base_vertex + pcmd->VtxOffset);
    }
  }
}

void GPUDevice::UploadVertexBuffer(const void* vertices, u32 vertex_size, u32 vertex_count, u32* base_vertex)
{
  void* map;
  u32 space;
  MapVertexBuffer(vertex_size, vertex_count, &map, &space, base_vertex);
  std::memcpy(map, vertices, vertex_size * vertex_count);
  UnmapVertexBuffer(vertex_size, vertex_count);
}

void GPUDevice::UploadIndexBuffer(const u16* indices, u32 index_count, u32* base_index)
{
  u16* map;
  u32 space;
  MapIndexBuffer(index_count, &map, &space, base_index);
  std::memcpy(map, indices, sizeof(u16) * index_count);
  UnmapIndexBuffer(index_count);
}

void GPUDevice::UploadUniformBuffer(const void* data, u32 data_size)
{
  void* map = MapUniformBuffer(data_size);
  std::memcpy(map, data, data_size);
  UnmapUniformBuffer(data_size);
}

void GPUDevice::SetRenderTarget(GPUTexture* rt, GPUTexture* ds /*= nullptr*/)
{
  SetRenderTargets(rt ? &rt : nullptr, rt ? 1 : 0, ds);
}

void GPUDevice::SetViewportAndScissor(s32 x, s32 y, s32 width, s32 height)
{
  SetViewport(x, y, width, height);
  SetScissor(x, y, width, height);
}

void GPUDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  t->SetClearColor(c);
}

void GPUDevice::ClearDepth(GPUTexture* t, float d)
{
  t->SetClearDepth(d);
}

void GPUDevice::InvalidateRenderTarget(GPUTexture* t)
{
  t->SetState(GPUTexture::State::Invalidated);
}

std::unique_ptr<GPUShader> GPUDevice::CreateShader(GPUShaderStage stage, const std::string_view& source,
                                                   const char* entry_point /* = "main" */)
{
  std::unique_ptr<GPUShader> shader;
  if (!m_shader_cache.IsOpen())
  {
    shader = CreateShaderFromSource(stage, source, entry_point, nullptr);
    return shader;
  }

  const GPUShaderCache::CacheIndexKey key = m_shader_cache.GetCacheKey(stage, source, entry_point);
  DynamicHeapArray<u8> binary;
  if (m_shader_cache.Lookup(key, &binary))
  {
    shader = CreateShaderFromBinary(stage, binary);
    if (shader)
      return shader;

    Log_ErrorPrintf("Failed to create shader from binary (driver changed?). Clearing cache.");
    m_shader_cache.Clear();
  }

  shader = CreateShaderFromSource(stage, source, entry_point, &binary);
  if (!shader)
    return shader;

  // Don't insert empty shaders into the cache...
  if (!binary.empty())
  {
    if (!m_shader_cache.Insert(key, binary.data(), static_cast<u32>(binary.size())))
      m_shader_cache.Close();
  }

  return shader;
}

bool GPUDevice::GetRequestedExclusiveFullscreenMode(u32* width, u32* height, float* refresh_rate)
{
  const std::string mode = Host::GetBaseStringSettingValue("GPU", "FullscreenMode", "");
  if (!mode.empty())
  {
    const std::string_view mode_view = mode;
    std::string_view::size_type sep1 = mode.find('x');
    if (sep1 != std::string_view::npos)
    {
      std::optional<u32> owidth = StringUtil::FromChars<u32>(mode_view.substr(0, sep1));
      sep1++;

      while (sep1 < mode.length() && std::isspace(mode[sep1]))
        sep1++;

      if (owidth.has_value() && sep1 < mode.length())
      {
        std::string_view::size_type sep2 = mode.find('@', sep1);
        if (sep2 != std::string_view::npos)
        {
          std::optional<u32> oheight = StringUtil::FromChars<u32>(mode_view.substr(sep1, sep2 - sep1));
          sep2++;

          while (sep2 < mode.length() && std::isspace(mode[sep2]))
            sep2++;

          if (oheight.has_value() && sep2 < mode.length())
          {
            std::optional<float> orefresh_rate = StringUtil::FromChars<float>(mode_view.substr(sep2));
            if (orefresh_rate.has_value())
            {
              *width = owidth.value();
              *height = oheight.value();
              *refresh_rate = orefresh_rate.value();
              return true;
            }
          }
        }
      }
    }
  }

  *width = 0;
  *height = 0;
  *refresh_rate = 0;
  return false;
}

std::string GPUDevice::GetFullscreenModeString(u32 width, u32 height, float refresh_rate)
{
  return fmt::format("{} x {} @ {} hz", width, height, refresh_rate);
}

std::string GPUDevice::GetShaderDumpPath(const std::string_view& name)
{
  return Path::Combine(EmuFolders::Dumps, name);
}

std::array<float, 4> GPUDevice::RGBA8ToFloat(u32 rgba)
{
  return std::array<float, 4>{static_cast<float>(rgba & UINT32_C(0xFF)) * (1.0f / 255.0f),
                              static_cast<float>((rgba >> 8) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                              static_cast<float>((rgba >> 16) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                              static_cast<float>(rgba >> 24) * (1.0f / 255.0f)};
}

bool GPUDevice::UpdateImGuiFontTexture()
{
  ImGuiIO& io = ImGui::GetIO();

  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  const u32 pitch = sizeof(u32) * width;

  if (m_imgui_font_texture && m_imgui_font_texture->GetWidth() == static_cast<u32>(width) &&
      m_imgui_font_texture->GetHeight() == static_cast<u32>(height) &&
      m_imgui_font_texture->Update(0, 0, static_cast<u32>(width), static_cast<u32>(height), pixels, pitch))
  {
    io.Fonts->SetTexID(m_imgui_font_texture.get());
    return true;
  }

  std::unique_ptr<GPUTexture> new_font =
    CreateTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8, pixels, pitch);
  if (!new_font)
    return false;

  m_imgui_font_texture = std::move(new_font);
  io.Fonts->SetTexID(m_imgui_font_texture.get());
  return true;
}

bool GPUDevice::UsesLowerLeftOrigin() const
{
  const RenderAPI api = GetRenderAPI();
  return (api == RenderAPI::OpenGL || api == RenderAPI::OpenGLES);
}

void GPUDevice::SetDisplayMaxFPS(float max_fps)
{
  m_display_frame_interval = (max_fps > 0.0f) ? (1.0f / max_fps) : 0.0f;
}

bool GPUDevice::ShouldSkipDisplayingFrame()
{
  if (m_display_frame_interval == 0.0f)
    return false;

  const u64 now = Common::Timer::GetCurrentValue();
  const double diff = Common::Timer::ConvertValueToSeconds(now - m_last_frame_displayed_time);
  if (diff < m_display_frame_interval)
    return true;

  m_last_frame_displayed_time = now;
  return false;
}

void GPUDevice::ThrottlePresentation()
{
  const float throttle_rate = (m_window_info.surface_refresh_rate > 0.0f) ? m_window_info.surface_refresh_rate : 60.0f;

  const u64 sleep_period = Common::Timer::ConvertNanosecondsToValue(1e+9f / static_cast<double>(throttle_rate));
  const u64 current_ts = Common::Timer::GetCurrentValue();

  // Allow it to fall behind/run ahead up to 2*period. Sleep isn't that precise, plus we need to
  // allow time for the actual rendering.
  const u64 max_variance = sleep_period * 2;
  if (static_cast<u64>(std::abs(static_cast<s64>(current_ts - m_last_frame_displayed_time))) > max_variance)
    m_last_frame_displayed_time = current_ts + sleep_period;
  else
    m_last_frame_displayed_time += sleep_period;

  Common::Timer::SleepUntil(m_last_frame_displayed_time, false);
}

bool GPUDevice::GetHostRefreshRate(float* refresh_rate)
{
  if (m_window_info.surface_refresh_rate > 0.0f)
  {
    *refresh_rate = m_window_info.surface_refresh_rate;
    return true;
  }

  return WindowInfo::QueryRefreshRateForWindow(m_window_info, refresh_rate);
}

bool GPUDevice::SetGPUTimingEnabled(bool enabled)
{
  return false;
}

float GPUDevice::GetAndResetAccumulatedGPUTime()
{
  return 0.0f;
}

std::unique_ptr<GPUDevice> GPUDevice::CreateDeviceForAPI(RenderAPI api)
{
  switch (api)
  {
#ifdef ENABLE_VULKAN
    case RenderAPI::Vulkan:
      return std::make_unique<VulkanDevice>();
#endif

#ifdef ENABLE_OPENGL
    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
      return std::make_unique<OpenGLDevice>();
#endif

#ifdef _WIN32
    case RenderAPI::D3D12:
      return std::make_unique<D3D12Device>();

    case RenderAPI::D3D11:
      return std::make_unique<D3D11Device>();
#endif

#ifdef __APPLE__
    case RenderAPI::Metal:
      return WrapNewMetalDevice();
#endif

    default:
      return {};
  }
}
