// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_device.h"
#include "compress_helpers.h"
#include "core/host.h"     // TODO: Remove, needed for getting fullscreen mode.
#include "core/settings.h" // TODO: Remove, needed for dump directory.
#include "gpu_framebuffer_manager.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "fmt/format.h"
#include "imgui.h"
#include "shaderc/shaderc.h"
#include "spirv_cross/spirv_cross_c.h"
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
static size_t s_pipeline_cache_size;
static std::array<u8, SHA1Digest::DIGEST_SIZE> s_pipeline_cache_hash;
size_t GPUDevice::s_total_vram_usage = 0;
GPUDevice::Statistics GPUDevice::s_stats = {};

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

u32 GPUPipeline::GraphicsConfig::GetRenderTargetCount() const
{
  u32 num_rts = 0;
  for (; num_rts < static_cast<u32>(std::size(color_formats)); num_rts++)
  {
    if (color_formats[num_rts] == GPUTexture::Format::Unknown)
      break;
  }
  return num_rts;
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

GPUDevice::GPUDevice()
{
  ResetStatistics();
}

GPUDevice::~GPUDevice() = default;

RenderAPI GPUDevice::GetPreferredAPI()
{
  static RenderAPI preferred_renderer = RenderAPI::None;
  if (preferred_renderer == RenderAPI::None) [[unlikely]]
  {
#if defined(_WIN32) && !defined(_M_ARM64)
    // Perfer DX11 on Windows, except ARM64, where QCom has slow DX11 drivers.
    preferred_renderer = RenderAPI::D3D11;
#elif defined(_WIN32) && defined(_M_ARM64)
    preferred_renderer = RenderAPI::D3D12;
#elif defined(__APPLE__)
    // Prefer Metal on MacOS.
    preferred_renderer = RenderAPI::Metal;
#elif defined(ENABLE_OPENGL) && defined(ENABLE_VULKAN)
    // On Linux, if we have both GL and Vulkan, prefer VK if the driver isn't software.
    preferred_renderer = VulkanDevice::IsSuitableDefaultRenderer() ? RenderAPI::Vulkan : RenderAPI::OpenGL;
#elif defined(ENABLE_OPENGL)
    preferred_renderer = RenderAPI::OpenGL;
#elif defined(ENABLE_VULKAN)
    preferred_renderer = RenderAPI::Vulkan;
#else
    // Uhhh, what?
    ERROR_LOG("Somehow don't have any renderers available...");
    preferred_renderer = RenderAPI::None;
#endif
  }

  return preferred_renderer;
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

const char* GPUDevice::ShaderLanguageToString(GPUShaderLanguage language)
{
  switch (language)
  {
    // clang-format off
#define CASE(x) case GPUShaderLanguage::x: return #x
    CASE(HLSL);
    CASE(GLSL);
    CASE(GLSLES);
    CASE(MSL);
    CASE(SPV);
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

GPUDevice::AdapterInfoList GPUDevice::GetAdapterListForAPI(RenderAPI api)
{
  AdapterInfoList ret;

  switch (api)
  {
#ifdef ENABLE_VULKAN
    case RenderAPI::Vulkan:
      ret = VulkanDevice::GetAdapterList();
      break;
#endif

#ifdef ENABLE_OPENGL
    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
      // No way of querying.
      break;
#endif

#ifdef _WIN32
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
      ret = D3DCommon::GetAdapterInfoList();
      break;
#endif

#ifdef __APPLE__
    case RenderAPI::Metal:
      ret = WrapGetMetalAdapterList();
      break;
#endif

    default:
      break;
  }

  return ret;
}

bool GPUDevice::Create(std::string_view adapter, std::string_view shader_cache_path, u32 shader_cache_version,
                       bool debug_device, GPUVSyncMode vsync, bool allow_present_throttle,
                       std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features, Error* error)
{
  m_vsync_mode = vsync;
  m_allow_present_throttle = allow_present_throttle;
  m_debug_device = debug_device;

  if (!AcquireWindow(true))
  {
    Error::SetStringView(error, "Failed to acquire window from host.");
    return false;
  }

  if (!CreateDevice(adapter, exclusive_fullscreen_control, disabled_features, error))
  {
    if (error && !error->IsValid())
      error->SetStringView("Failed to create device.");
    return false;
  }

  INFO_LOG("Render API: {} Version {}", RenderAPIToString(m_render_api), m_render_api_version);
  INFO_LOG("Graphics Driver Info:\n{}", GetDriverInfo());

  OpenShaderCache(shader_cache_path, shader_cache_version);

  if (!CreateResources(error))
  {
    Error::AddPrefix(error, "Failed to create base resources.");
    return false;
  }

  return true;
}

void GPUDevice::Destroy()
{
  PurgeTexturePool();
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

void GPUDevice::OpenShaderCache(std::string_view base_path, u32 version)
{
  if (m_features.shader_cache && !base_path.empty())
  {
    const std::string basename = GetShaderCacheBaseName("shaders");
    const std::string filename = Path::Combine(base_path, basename);
    if (!m_shader_cache.Open(filename.c_str(), m_render_api_version, version))
    {
      WARNING_LOG("Failed to open shader cache. Creating new cache.");
      if (!m_shader_cache.Create())
        ERROR_LOG("Failed to create new shader cache.");

      // Squish the pipeline cache too, it's going to be stale.
      if (m_features.pipeline_cache)
      {
        const std::string pc_filename =
          Path::Combine(base_path, TinyString::from_format("{}.bin", GetShaderCacheBaseName("pipelines")));
        if (FileSystem::FileExists(pc_filename.c_str()))
        {
          INFO_LOG("Removing old pipeline cache '{}'", Path::GetFileName(pc_filename));
          FileSystem::DeleteFile(pc_filename.c_str());
        }
      }
    }
  }
  else
  {
    // Still need to set the version - GL needs it.
    m_shader_cache.Open(std::string_view(), m_render_api_version, version);
  }

  s_pipeline_cache_path = {};
  s_pipeline_cache_size = 0;
  s_pipeline_cache_hash = {};

  if (m_features.pipeline_cache && !base_path.empty())
  {
    Error error;
    s_pipeline_cache_path =
      Path::Combine(base_path, TinyString::from_format("{}.bin", GetShaderCacheBaseName("pipelines")));
    if (FileSystem::FileExists(s_pipeline_cache_path.c_str()))
    {
      if (OpenPipelineCache(s_pipeline_cache_path, &error))
        return;

      WARNING_LOG("Failed to read pipeline cache '{}': {}", Path::GetFileName(s_pipeline_cache_path),
                  error.GetDescription());
    }

    if (!CreatePipelineCache(s_pipeline_cache_path, &error))
    {
      WARNING_LOG("Failed to create pipeline cache '{}': {}", Path::GetFileName(s_pipeline_cache_path),
                  error.GetDescription());
      s_pipeline_cache_path = {};
    }
  }
}

void GPUDevice::CloseShaderCache()
{
  m_shader_cache.Close();

  if (!s_pipeline_cache_path.empty())
  {
    Error error;
    if (!ClosePipelineCache(s_pipeline_cache_path, &error))
    {
      WARNING_LOG("Failed to close pipeline cache '{}': {}", Path::GetFileName(s_pipeline_cache_path),
                  error.GetDescription());
    }

    s_pipeline_cache_path = {};
  }
}

std::string GPUDevice::GetShaderCacheBaseName(std::string_view type) const
{
  const std::string_view debug_suffix = m_debug_device ? "_debug" : "";

  TinyString lower_api_name(RenderAPIToString(m_render_api));
  lower_api_name.convert_to_lower_case();

  return fmt::format("{}_{}{}", lower_api_name, type, debug_suffix);
}

bool GPUDevice::OpenPipelineCache(const std::string& path, Error* error)
{
  CompressHelpers::OptionalByteBuffer data =
    CompressHelpers::DecompressFile(CompressHelpers::CompressType::Zstandard, path.c_str(), std::nullopt, error);
  if (!data.has_value())
    return false;

  const size_t cache_size = data->size();
  const std::array<u8, SHA1Digest::DIGEST_SIZE> cache_hash = SHA1Digest::GetDigest(data->cspan());

  INFO_LOG("Loading {} byte pipeline cache with hash {}", s_pipeline_cache_size,
           SHA1Digest::DigestToString(s_pipeline_cache_hash));

  if (!ReadPipelineCache(std::move(data.value()), error))
    return false;

  s_pipeline_cache_size = cache_size;
  s_pipeline_cache_hash = cache_hash;
  return true;
}

bool GPUDevice::CreatePipelineCache(const std::string& path, Error* error)
{
  return false;
}

bool GPUDevice::ClosePipelineCache(const std::string& path, Error* error)
{
  DynamicHeapArray<u8> data;
  if (!GetPipelineCacheData(&data, error))
    return false;

  // Save disk writes if it hasn't changed, think of the poor SSDs.
  if (s_pipeline_cache_size == data.size() && s_pipeline_cache_hash == SHA1Digest::GetDigest(data.cspan()))
  {
    INFO_LOG("Skipping updating pipeline cache '{}' due to no changes.", Path::GetFileName(path));
    return true;
  }

  INFO_LOG("Compressing and writing {} bytes to '{}'", data.size(), Path::GetFileName(path));
  return CompressHelpers::CompressToFile(CompressHelpers::CompressType::Zstandard, path.c_str(), data.cspan(), -1, true,
                                         error);
}

bool GPUDevice::ReadPipelineCache(DynamicHeapArray<u8> data, Error* error)
{
  return false;
}

bool GPUDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data, Error* error)
{
  return false;
}

bool GPUDevice::AcquireWindow(bool recreate_window)
{
  std::optional<WindowInfo> wi = Host::AcquireRenderWindow(recreate_window);
  if (!wi.has_value())
    return false;

  INFO_LOG("Render window is {}x{}.", wi->surface_width, wi->surface_height);
  m_window_info = wi.value();
  return true;
}

bool GPUDevice::CreateResources(Error* error)
{
  if (!(m_nearest_sampler = CreateSampler(GPUSampler::GetNearestConfig())) ||
      !(m_linear_sampler = CreateSampler(GPUSampler::GetLinearConfig())))
  {
    Error::SetStringView(error, "Failed to create samplers");
    return false;
  }

  const RenderAPI render_api = GetRenderAPI();
  ShaderGen shadergen(render_api, ShaderGen::GetShaderLanguageForAPI(render_api), m_features.dual_source_blend,
                      m_features.framebuffer_fetch);

  std::unique_ptr<GPUShader> imgui_vs =
    CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateImGuiVertexShader(), error);
  std::unique_ptr<GPUShader> imgui_fs =
    CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateImGuiFragmentShader(), error);
  if (!imgui_vs || !imgui_fs)
  {
    Error::AddPrefix(error, "Failed to compile ImGui shaders: ");
    return false;
  }
  GL_OBJECT_NAME(imgui_vs, "ImGui Vertex Shader");
  GL_OBJECT_NAME(imgui_fs, "ImGui Fragment Shader");

  static constexpr GPUPipeline::VertexAttribute imgui_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ImDrawVert, pos)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ImDrawVert, uv)),
    GPUPipeline::VertexAttribute::Make(2, GPUPipeline::VertexAttribute::Semantic::Color, 0,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(ImDrawVert, col)),
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
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = imgui_vs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.fragment_shader = imgui_fs.get();

  m_imgui_pipeline = CreatePipeline(plconfig, error);
  if (!m_imgui_pipeline)
  {
    Error::AddPrefix(error, "Failed to compile ImGui pipeline: ");
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
  const bool flip = UsesLowerLeftOrigin();
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

      if (flip)
      {
        const s32 height = static_cast<s32>(pcmd->ClipRect.w - pcmd->ClipRect.y);
        const s32 flipped_y =
          static_cast<s32>(m_window_info.surface_height) - static_cast<s32>(pcmd->ClipRect.y) - height;
        SetScissor(static_cast<s32>(pcmd->ClipRect.x), flipped_y, static_cast<s32>(pcmd->ClipRect.z - pcmd->ClipRect.x),
                   height);
      }
      else
      {
        SetScissor(static_cast<s32>(pcmd->ClipRect.x), static_cast<s32>(pcmd->ClipRect.y),
                   static_cast<s32>(pcmd->ClipRect.z - pcmd->ClipRect.x),
                   static_cast<s32>(pcmd->ClipRect.w - pcmd->ClipRect.y));
      }

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

void GPUDevice::SetRenderTarget(GPUTexture* rt, GPUTexture* ds, GPUPipeline::RenderPassFlag render_pass_flags)
{
  SetRenderTargets(rt ? &rt : nullptr, rt ? 1 : 0, ds, render_pass_flags);
}

void GPUDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  SetViewport(GSVector4i(x, y, x + width, y + height));
}

void GPUDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  SetScissor(GSVector4i(x, y, x + width, y + height));
}

void GPUDevice::SetViewportAndScissor(s32 x, s32 y, s32 width, s32 height)
{
  SetViewportAndScissor(GSVector4i(x, y, x + width, y + height));
}

void GPUDevice::SetViewportAndScissor(const GSVector4i rc)
{
  SetViewport(rc);
  SetScissor(rc);
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

std::unique_ptr<GPUShader> GPUDevice::CreateShader(GPUShaderStage stage, GPUShaderLanguage language,
                                                   std::string_view source, Error* error /* = nullptr */,
                                                   const char* entry_point /* = "main" */)
{
  std::unique_ptr<GPUShader> shader;
  if (!m_shader_cache.IsOpen())
  {
    shader = CreateShaderFromSource(stage, language, source, entry_point, nullptr, error);
    return shader;
  }

  const GPUShaderCache::CacheIndexKey key = m_shader_cache.GetCacheKey(stage, language, source, entry_point);
  std::optional<GPUShaderCache::ShaderBinary> binary = m_shader_cache.Lookup(key);
  if (binary.has_value())
  {
    shader = CreateShaderFromBinary(stage, binary->cspan(), error);
    if (shader)
      return shader;

    ERROR_LOG("Failed to create shader from binary (driver changed?). Clearing cache.");
    m_shader_cache.Clear();
    binary.reset();
  }

  GPUShaderCache::ShaderBinary new_binary;
  shader = CreateShaderFromSource(stage, language, source, entry_point, &new_binary, error);
  if (!shader)
    return shader;

  // Don't insert empty shaders into the cache...
  if (!new_binary.empty())
  {
    if (!m_shader_cache.Insert(key, new_binary.data(), static_cast<u32>(new_binary.size())))
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

std::string GPUDevice::GetShaderDumpPath(std::string_view name)
{
  return Path::Combine(EmuFolders::Dumps, name);
}

void GPUDevice::DumpBadShader(std::string_view code, std::string_view errors)
{
  static u32 next_bad_shader_id = 0;

  const std::string filename = GetShaderDumpPath(fmt::format("bad_shader_{}.txt", ++next_bad_shader_id));
  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb");
  if (fp)
  {
    if (!code.empty())
      std::fwrite(code.data(), code.size(), 1, fp.get());
    std::fputs("\n\n**** ERRORS ****\n", fp.get());
    if (!errors.empty())
      std::fwrite(errors.data(), errors.size(), 1, fp.get());
  }
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
    FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8, pixels, pitch);
  if (!new_font)
    return false;

  RecycleTexture(std::move(m_imgui_font_texture));
  m_imgui_font_texture = std::move(new_font);
  io.Fonts->SetTexID(m_imgui_font_texture.get());
  return true;
}

bool GPUDevice::UsesLowerLeftOrigin() const
{
  const RenderAPI api = GetRenderAPI();
  return (api == RenderAPI::OpenGL || api == RenderAPI::OpenGLES);
}

GSVector4i GPUDevice::FlipToLowerLeft(GSVector4i rc, s32 target_height)
{
  const s32 height = rc.height();
  const s32 flipped_y = target_height - rc.top - height;
  rc.top = flipped_y;
  rc.bottom = flipped_y + height;
  return rc;
}

bool GPUDevice::IsTexturePoolType(GPUTexture::Type type)
{
  return (type == GPUTexture::Type::Texture || type == GPUTexture::Type::DynamicTexture);
}

std::unique_ptr<GPUTexture> GPUDevice::FetchTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Type type, GPUTexture::Format format,
                                                    const void* data /*= nullptr*/, u32 data_stride /*= 0*/)
{
  std::unique_ptr<GPUTexture> ret;

  const TexturePoolKey key = {static_cast<u16>(width),
                              static_cast<u16>(height),
                              static_cast<u8>(layers),
                              static_cast<u8>(levels),
                              static_cast<u8>(samples),
                              type,
                              format,
                              0u};

  const bool is_texture = IsTexturePoolType(type);
  TexturePool& pool = is_texture ? m_texture_pool : m_target_pool;
  const u32 pool_size = (is_texture ? MAX_TEXTURE_POOL_SIZE : MAX_TARGET_POOL_SIZE);

  TexturePool::iterator it;

  if (is_texture && m_features.prefer_unused_textures)
  {
    // Try to find a texture that wasn't used this frame first.
    for (it = m_texture_pool.begin(); it != m_texture_pool.end(); ++it)
    {
      if (it->use_counter == m_texture_pool_counter)
      {
        // We're into textures recycled this frame, not going to find anything newer.
        // But prefer reuse over creating a new texture.
        if (m_texture_pool.size() < pool_size)
        {
          it = m_texture_pool.end();
          break;
        }
      }

      if (it->key == key)
        break;
    }
  }
  else
  {
    for (it = pool.begin(); it != pool.end(); ++it)
    {
      if (it->key == key)
        break;
    }
  }

  if (it != pool.end())
  {
    if (!data || it->texture->Update(0, 0, width, height, data, data_stride, 0, 0))
    {
      ret = std::move(it->texture);
      pool.erase(it);
      return ret;
    }
    else
    {
      // This shouldn't happen...
      ERROR_LOG("Failed to upload {}x{} to pooled texture", width, height);
    }
  }

  ret = CreateTexture(width, height, layers, levels, samples, type, format, data, data_stride);
  return ret;
}

std::unique_ptr<GPUTexture, GPUDevice::PooledTextureDeleter>
GPUDevice::FetchAutoRecycleTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, GPUTexture::Type type,
                                   GPUTexture::Format format, const void* data /*= nullptr*/, u32 data_stride /*= 0*/,
                                   bool dynamic /*= false*/)
{
  std::unique_ptr<GPUTexture> ret =
    FetchTexture(width, height, layers, levels, samples, type, format, data, data_stride);
  return std::unique_ptr<GPUTexture, PooledTextureDeleter>(ret.release());
}

void GPUDevice::RecycleTexture(std::unique_ptr<GPUTexture> texture)
{
  if (!texture)
    return;

  const TexturePoolKey key = {static_cast<u16>(texture->GetWidth()),
                              static_cast<u16>(texture->GetHeight()),
                              static_cast<u8>(texture->GetLayers()),
                              static_cast<u8>(texture->GetLevels()),
                              static_cast<u8>(texture->GetSamples()),
                              texture->GetType(),
                              texture->GetFormat(),
                              0u};

  const bool is_texture = IsTexturePoolType(texture->GetType());
  TexturePool& pool = is_texture ? m_texture_pool : m_target_pool;
  pool.push_back({std::move(texture), m_texture_pool_counter, key});

  const u32 max_size = is_texture ? MAX_TEXTURE_POOL_SIZE : MAX_TARGET_POOL_SIZE;
  while (pool.size() > max_size)
  {
    DEBUG_LOG("Trim {}x{} texture from pool", pool.front().texture->GetWidth(), pool.front().texture->GetHeight());
    pool.pop_front();
  }
}

void GPUDevice::PurgeTexturePool()
{
  m_texture_pool_counter = 0;
  m_texture_pool.clear();
  m_target_pool.clear();
}

void GPUDevice::TrimTexturePool()
{
  GL_INS_FMT("Texture Pool Size: {}", m_texture_pool.size());
  GL_INS_FMT("Target Pool Size: {}", m_target_pool.size());
  GL_INS_FMT("VRAM Usage: {:.2f} MB", s_total_vram_usage / 1048576.0);

  DEBUG_LOG("Texture Pool Size: {} Target Pool Size: {} VRAM: {:.2f} MB", m_texture_pool.size(), m_target_pool.size(),
            s_total_vram_usage / 1048756.0);

  if (m_texture_pool.empty() && m_target_pool.empty())
    return;

  const u32 prev_counter = m_texture_pool_counter++;
  for (u32 pool_idx = 0; pool_idx < 2; pool_idx++)
  {
    TexturePool& pool = pool_idx ? m_target_pool : m_texture_pool;
    for (auto it = pool.begin(); it != pool.end();)
    {
      const u32 delta = (prev_counter - it->use_counter);
      if (delta < POOL_PURGE_DELAY)
        break;

      DEBUG_LOG("Trim {}x{} texture from pool", it->texture->GetWidth(), it->texture->GetHeight());
      it = pool.erase(it);
    }
  }

  if (m_texture_pool_counter < prev_counter) [[unlikely]]
  {
    // wrapped around, handle it
    if (m_texture_pool.empty() && m_target_pool.empty())
    {
      m_texture_pool_counter = 0;
    }
    else
    {
      const u32 texture_min =
        m_texture_pool.empty() ? std::numeric_limits<u32>::max() : m_texture_pool.front().use_counter;
      const u32 target_min =
        m_target_pool.empty() ? std::numeric_limits<u32>::max() : m_target_pool.front().use_counter;
      const u32 reduce = std::min(texture_min, target_min);
      m_texture_pool_counter -= reduce;
      for (u32 pool_idx = 0; pool_idx < 2; pool_idx++)
      {
        TexturePool& pool = pool_idx ? m_target_pool : m_texture_pool;
        for (TexturePoolEntry& entry : pool)
          entry.use_counter -= reduce;
      }
    }
  }
}

bool GPUDevice::ResizeTexture(std::unique_ptr<GPUTexture>* tex, u32 new_width, u32 new_height, GPUTexture::Type type,
                              GPUTexture::Format format, bool preserve /* = true */)
{
  GPUTexture* old_tex = tex->get();
  DebugAssert(!old_tex || (old_tex->GetLayers() == 1 && old_tex->GetLevels() == 1 && old_tex->GetSamples() == 1));
  std::unique_ptr<GPUTexture> new_tex = FetchTexture(new_width, new_height, 1, 1, 1, type, format);
  if (!new_tex) [[unlikely]]
  {
    ERROR_LOG("Failed to create new {}x{} texture", new_width, new_height);
    return false;
  }

  if (old_tex)
  {
    if (old_tex->GetState() == GPUTexture::State::Cleared)
    {
      if (type == GPUTexture::Type::RenderTarget)
        ClearRenderTarget(new_tex.get(), old_tex->GetClearColor());
    }
    else if (old_tex->GetState() == GPUTexture::State::Dirty)
    {
      const u32 copy_width = std::min(new_width, old_tex->GetWidth());
      const u32 copy_height = std::min(new_height, old_tex->GetHeight());
      if (type == GPUTexture::Type::RenderTarget)
        ClearRenderTarget(new_tex.get(), 0);
      CopyTextureRegion(new_tex.get(), 0, 0, 0, 0, old_tex, 0, 0, 0, 0, copy_width, copy_height);
    }
  }
  else if (preserve)
  {
    // If we're expecting data to be there, make sure to clear it.
    if (type == GPUTexture::Type::RenderTarget)
      ClearRenderTarget(new_tex.get(), 0);
  }

  RecycleTexture(std::move(*tex));
  *tex = std::move(new_tex);
  return true;
}

bool GPUDevice::ShouldSkipPresentingFrame()
{
  // Only needed with FIFO. But since we're so fast, we allow it always.
  if (!m_allow_present_throttle)
    return false;

  const float throttle_rate = (m_window_info.surface_refresh_rate > 0.0f) ? m_window_info.surface_refresh_rate : 60.0f;
  const float throttle_period = 1.0f / throttle_rate;

  const u64 now = Common::Timer::GetCurrentValue();
  const double diff = Common::Timer::ConvertValueToSeconds(now - m_last_frame_displayed_time);
  if (diff < throttle_period)
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

bool GPUDevice::SetGPUTimingEnabled(bool enabled)
{
  return false;
}

float GPUDevice::GetAndResetAccumulatedGPUTime()
{
  return 0.0f;
}

void GPUDevice::ResetStatistics()
{
  s_stats = {};
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

#ifndef _WIN32
// Use a duckstation-suffixed shaderc name to avoid conflicts and loading another shaderc, e.g. from the Vulkan SDK.
#define SHADERC_LIB_NAME "shaderc_ds"
#else
#define SHADERC_LIB_NAME "shaderc_shared"
#endif

#define SHADERC_FUNCTIONS(X)                                                                                           \
  X(shaderc_compiler_initialize)                                                                                       \
  X(shaderc_compiler_release)                                                                                          \
  X(shaderc_compile_options_initialize)                                                                                \
  X(shaderc_compile_options_release)                                                                                   \
  X(shaderc_compile_options_set_source_language)                                                                       \
  X(shaderc_compile_options_set_generate_debug_info)                                                                   \
  X(shaderc_compile_options_set_optimization_level)                                                                    \
  X(shaderc_compile_options_set_target_env)                                                                            \
  X(shaderc_compilation_status_to_string)                                                                              \
  X(shaderc_compile_into_spv)                                                                                          \
  X(shaderc_result_release)                                                                                            \
  X(shaderc_result_get_length)                                                                                         \
  X(shaderc_result_get_num_warnings)                                                                                   \
  X(shaderc_result_get_bytes)                                                                                          \
  X(shaderc_result_get_compilation_status)                                                                             \
  X(shaderc_result_get_error_message)                                                                                  \
  X(shaderc_optimize_spv)

#define SPIRV_CROSS_FUNCTIONS(X)                                                                                       \
  X(spvc_context_create)                                                                                               \
  X(spvc_context_destroy)                                                                                              \
  X(spvc_context_set_error_callback)                                                                                   \
  X(spvc_context_parse_spirv)                                                                                          \
  X(spvc_context_create_compiler)                                                                                      \
  X(spvc_compiler_create_compiler_options)                                                                             \
  X(spvc_compiler_create_shader_resources)                                                                             \
  X(spvc_compiler_get_execution_model)                                                                                 \
  X(spvc_compiler_options_set_bool)                                                                                    \
  X(spvc_compiler_options_set_uint)                                                                                    \
  X(spvc_compiler_install_compiler_options)                                                                            \
  X(spvc_compiler_require_extension)                                                                                   \
  X(spvc_compiler_compile)                                                                                             \
  X(spvc_resources_get_resource_list_for_type)

#ifdef _WIN32
#define SPIRV_CROSS_HLSL_FUNCTIONS(X) X(spvc_compiler_hlsl_add_resource_binding)
#else
#define SPIRV_CROSS_HLSL_FUNCTIONS(X)
#endif
#ifdef __APPLE__
#define SPIRV_CROSS_MSL_FUNCTIONS(X) X(spvc_compiler_msl_add_resource_binding)
#else
#define SPIRV_CROSS_MSL_FUNCTIONS(X)
#endif

// TODO: NOT thread safe, yet.
namespace dyn_libs {
static bool OpenShaderc(Error* error);
static void CloseShaderc();
static bool OpenSpirvCross(Error* error);
static void CloseSpirvCross();
static void CloseAll();

static DynamicLibrary s_shaderc_library;
static DynamicLibrary s_spirv_cross_library;

static shaderc_compiler_t s_shaderc_compiler = nullptr;

static bool s_close_registered = false;

#define ADD_FUNC(F) static decltype(&::F) F;
SHADERC_FUNCTIONS(ADD_FUNC)
SPIRV_CROSS_FUNCTIONS(ADD_FUNC)
SPIRV_CROSS_HLSL_FUNCTIONS(ADD_FUNC)
SPIRV_CROSS_MSL_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

} // namespace dyn_libs

bool dyn_libs::OpenShaderc(Error* error)
{
  if (s_shaderc_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename(SHADERC_LIB_NAME);
  if (!s_shaderc_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load shaderc: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_shaderc_library.GetSymbol(#F, &F))                                                                            \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseShaderc();                                                                                                    \
    return false;                                                                                                      \
  }

  SHADERC_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  s_shaderc_compiler = shaderc_compiler_initialize();
  if (!s_shaderc_compiler)
  {
    Error::SetStringView(error, "shaderc_compiler_initialize() failed");
    CloseShaderc();
    return false;
  }

  if (!s_close_registered)
  {
    s_close_registered = true;
    std::atexit(&dyn_libs::CloseAll);
  }

  return true;
}

void dyn_libs::CloseShaderc()
{
  if (s_shaderc_compiler)
  {
    shaderc_compiler_release(s_shaderc_compiler);
    s_shaderc_compiler = nullptr;
  }

#define UNLOAD_FUNC(F) F = nullptr;
  SHADERC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_shaderc_library.Close();
}

bool dyn_libs::OpenSpirvCross(Error* error)
{
  if (s_spirv_cross_library.IsOpen())
    return true;

#ifdef _WIN32
  // SPVC's build on Windows doesn't spit out a versioned DLL.
  const std::string libname = DynamicLibrary::GetVersionedFilename("spirv-cross-c-shared");
#else
  const std::string libname = DynamicLibrary::GetVersionedFilename("spirv-cross-c-shared", SPVC_C_API_VERSION_MAJOR);
#endif
  if (!s_spirv_cross_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load spirv-cross: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_spirv_cross_library.GetSymbol(#F, &F))                                                                        \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseShaderc();                                                                                                    \
    return false;                                                                                                      \
  }

  SPIRV_CROSS_FUNCTIONS(LOAD_FUNC)
  SPIRV_CROSS_HLSL_FUNCTIONS(LOAD_FUNC)
  SPIRV_CROSS_MSL_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  if (!s_close_registered)
  {
    s_close_registered = true;
    std::atexit(&dyn_libs::CloseAll);
  }

  return true;
}

void dyn_libs::CloseSpirvCross()
{
#define UNLOAD_FUNC(F) F = nullptr;
  SPIRV_CROSS_FUNCTIONS(UNLOAD_FUNC)
  SPIRV_CROSS_HLSL_FUNCTIONS(UNLOAD_FUNC)
  SPIRV_CROSS_MSL_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_spirv_cross_library.Close();
}

void dyn_libs::CloseAll()
{
  CloseShaderc();
  CloseSpirvCross();
}

#undef SPIRV_CROSS_HLSL_FUNCTIONS
#undef SPIRV_CROSS_MSL_FUNCTIONS
#undef SPIRV_CROSS_FUNCTIONS
#undef SHADERC_FUNCTIONS

std::optional<DynamicHeapArray<u8>> GPUDevice::OptimizeVulkanSpv(const std::span<const u8> spirv, Error* error)
{
  std::optional<DynamicHeapArray<u8>> ret;

  if (spirv.size() < sizeof(u32) * 2)
  {
    Error::SetStringView(error, "Invalid SPIR-V input size.");
    return ret;
  }

  // Need to set environment based on version.
  u32 magic_word, spirv_version;
  shaderc_target_env target_env = shaderc_target_env_vulkan;
  shaderc_env_version target_version = shaderc_env_version_vulkan_1_0;
  std::memcpy(&magic_word, spirv.data(), sizeof(magic_word));
  std::memcpy(&spirv_version, spirv.data() + sizeof(magic_word), sizeof(spirv_version));
  if (magic_word != 0x07230203u)
  {
    Error::SetStringView(error, "Invalid SPIR-V magic word.");
    return ret;
  }
  if (spirv_version < 0x10300)
    target_version = shaderc_env_version_vulkan_1_0;
  else
    target_version = shaderc_env_version_vulkan_1_1;

  if (!dyn_libs::OpenShaderc(error))
    return ret;

  const shaderc_compile_options_t options = dyn_libs::shaderc_compile_options_initialize();
  AssertMsg(options, "shaderc_compile_options_initialize() failed");
  dyn_libs::shaderc_compile_options_set_target_env(options, target_env, target_version);
  dyn_libs::shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);

  const shaderc_compilation_result_t result =
    dyn_libs::shaderc_optimize_spv(dyn_libs::s_shaderc_compiler, spirv.data(), spirv.size(), options);
  const shaderc_compilation_status status =
    result ? dyn_libs::shaderc_result_get_compilation_status(result) : shaderc_compilation_status_internal_error;
  if (status != shaderc_compilation_status_success)
  {
    const std::string_view errors(result ? dyn_libs::shaderc_result_get_error_message(result) : "null result object");
    Error::SetStringFmt(error, "Failed to optimize SPIR-V: {}\n{}",
                        dyn_libs::shaderc_compilation_status_to_string(status), errors);
  }
  else
  {
    const size_t spirv_size = dyn_libs::shaderc_result_get_length(result);
    DebugAssert(spirv_size > 0);
    ret = DynamicHeapArray<u8>(spirv_size);
    std::memcpy(ret->data(), dyn_libs::shaderc_result_get_bytes(result), spirv_size);
  }

  dyn_libs::shaderc_result_release(result);
  dyn_libs::shaderc_compile_options_release(options);
  return ret;
}

bool GPUDevice::CompileGLSLShaderToVulkanSpv(GPUShaderStage stage, GPUShaderLanguage source_language,
                                             std::string_view source, const char* entry_point, bool optimization,
                                             bool nonsemantic_debug_info, DynamicHeapArray<u8>* out_binary,
                                             Error* error)
{
  static constexpr const std::array<shaderc_shader_kind, static_cast<size_t>(GPUShaderStage::MaxCount)> stage_kinds = {{
    shaderc_glsl_vertex_shader,
    shaderc_glsl_fragment_shader,
    shaderc_glsl_geometry_shader,
    shaderc_glsl_compute_shader,
  }};

  if (source_language != GPUShaderLanguage::GLSLVK)
  {
    Error::SetStringFmt(error, "Unsupported source language for transpile: {}",
                        ShaderLanguageToString(source_language));
    return false;
  }

  if (!dyn_libs::OpenShaderc(error))
    return false;

  const shaderc_compile_options_t options = dyn_libs::shaderc_compile_options_initialize();
  AssertMsg(options, "shaderc_compile_options_initialize() failed");

  dyn_libs::shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
  dyn_libs::shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, 0);
  dyn_libs::shaderc_compile_options_set_generate_debug_info(options, m_debug_device,
                                                            m_debug_device && nonsemantic_debug_info);
  dyn_libs::shaderc_compile_options_set_optimization_level(
    options, optimization ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);

  const shaderc_compilation_result_t result =
    dyn_libs::shaderc_compile_into_spv(dyn_libs::s_shaderc_compiler, source.data(), source.length(),
                                       stage_kinds[static_cast<size_t>(stage)], "source", entry_point, options);
  const shaderc_compilation_status status =
    result ? dyn_libs::shaderc_result_get_compilation_status(result) : shaderc_compilation_status_internal_error;
  if (status != shaderc_compilation_status_success)
  {
    const std::string_view errors(result ? dyn_libs::shaderc_result_get_error_message(result) : "null result object");
    Error::SetStringFmt(error, "Failed to compile shader to SPIR-V: {}\n{}",
                        dyn_libs::shaderc_compilation_status_to_string(status), errors);
    ERROR_LOG("Failed to compile shader to SPIR-V: {}\n{}", dyn_libs::shaderc_compilation_status_to_string(status),
              errors);
    DumpBadShader(source, errors);
  }
  else
  {
    const size_t num_warnings = dyn_libs::shaderc_result_get_num_warnings(result);
    if (num_warnings > 0)
      WARNING_LOG("Shader compiled with warnings:\n{}", dyn_libs::shaderc_result_get_error_message(result));

    const size_t spirv_size = dyn_libs::shaderc_result_get_length(result);
    DebugAssert(spirv_size > 0);
    out_binary->resize(spirv_size);
    std::memcpy(out_binary->data(), dyn_libs::shaderc_result_get_bytes(result), spirv_size);
  }

  dyn_libs::shaderc_result_release(result);
  dyn_libs::shaderc_compile_options_release(options);
  return (status == shaderc_compilation_status_success);
}

bool GPUDevice::TranslateVulkanSpvToLanguage(const std::span<const u8> spirv, GPUShaderStage stage,
                                             GPUShaderLanguage target_language, u32 target_version, std::string* output,
                                             Error* error)
{
  if (!dyn_libs::OpenSpirvCross(error))
    return false;

  spvc_context sctx;
  spvc_result sres;
  if ((sres = dyn_libs::spvc_context_create(&sctx)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_create() failed: {}", static_cast<int>(sres));
    return false;
  }

  const ScopedGuard sctx_guard = [&sctx]() { dyn_libs::spvc_context_destroy(sctx); };

  dyn_libs::spvc_context_set_error_callback(
    sctx,
    [](void* error, const char* errormsg) {
      ERROR_LOG("SPIRV-Cross reported an error: {}", errormsg);
      Error::SetStringView(static_cast<Error*>(error), errormsg);
    },
    error);

  spvc_parsed_ir sir;
  if ((sres = dyn_libs::spvc_context_parse_spirv(sctx, reinterpret_cast<const u32*>(spirv.data()), spirv.size() / 4,
                                                 &sir)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_parse_spirv() failed: {}", static_cast<int>(sres));
    return {};
  }

  static constexpr std::array<spvc_backend, static_cast<size_t>(GPUShaderLanguage::Count)> backends = {
    {SPVC_BACKEND_NONE, SPVC_BACKEND_HLSL, SPVC_BACKEND_GLSL, SPVC_BACKEND_GLSL, SPVC_BACKEND_GLSL, SPVC_BACKEND_MSL,
     SPVC_BACKEND_NONE}};

  spvc_compiler scompiler;
  if ((sres = dyn_libs::spvc_context_create_compiler(sctx, backends[static_cast<size_t>(target_language)], sir,
                                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &scompiler)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_create_compiler() failed: {}", static_cast<int>(sres));
    return {};
  }

  spvc_compiler_options soptions;
  if ((sres = dyn_libs::spvc_compiler_create_compiler_options(scompiler, &soptions)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_create_compiler_options() failed: {}", static_cast<int>(sres));
    return {};
  }

  spvc_resources resources;
  if ((sres = dyn_libs::spvc_compiler_create_shader_resources(scompiler, &resources)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_create_shader_resources() failed: {}", static_cast<int>(sres));
    return {};
  }

  // Need to know if there's UBOs for mapping.
  const spvc_reflected_resource *ubos, *textures;
  size_t ubos_count, textures_count;
  if ((sres = dyn_libs::spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &ubos,
                                                                  &ubos_count)) != SPVC_SUCCESS ||
      (sres = dyn_libs::spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
                                                                  &textures, &textures_count)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_resources_get_resource_list_for_type() failed: {}", static_cast<int>(sres));
    return {};
  }

  [[maybe_unused]] const SpvExecutionModel execmodel = dyn_libs::spvc_compiler_get_execution_model(scompiler);
  [[maybe_unused]] static constexpr u32 UBO_DESCRIPTOR_SET = 0;
  [[maybe_unused]] static constexpr u32 TEXTURE_DESCRIPTOR_SET = 1;

  switch (target_language)
  {
#ifdef _WIN32
    case GPUShaderLanguage::HLSL:
    {
      if ((sres = dyn_libs::spvc_compiler_options_set_uint(soptions, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL,
                                                           target_version)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(error, "spvc_compiler_options_set_uint(SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      if ((sres = dyn_libs::spvc_compiler_options_set_bool(
             soptions, SPVC_COMPILER_OPTION_HLSL_SUPPORT_NONZERO_BASE_VERTEX_BASE_INSTANCE, false)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(error,
                            "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_HLSL_SUPPORT_NONZERO_BASE_VERTEX_"
                            "BASE_INSTANCE) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      if ((sres = dyn_libs::spvc_compiler_options_set_bool(soptions, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT,
                                                           true)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(error,
                            "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      if (ubos_count > 0)
      {
        const spvc_hlsl_resource_binding rb = {.stage = execmodel,
                                               .desc_set = UBO_DESCRIPTOR_SET,
                                               .binding = 0,
                                               .cbv = {.register_space = 0, .register_binding = 0}};
        if ((sres = dyn_libs::spvc_compiler_hlsl_add_resource_binding(scompiler, &rb)) != SPVC_SUCCESS)
        {
          Error::SetStringFmt(error, "spvc_compiler_hlsl_add_resource_binding() failed: {}", static_cast<int>(sres));
          return {};
        }
      }

      if (textures_count > 0)
      {
        for (u32 i = 0; i < textures_count; i++)
        {
          const spvc_hlsl_resource_binding rb = {.stage = execmodel,
                                                 .desc_set = TEXTURE_DESCRIPTOR_SET,
                                                 .binding = i,
                                                 .srv = {.register_space = 0, .register_binding = i},
                                                 .sampler = {.register_space = 0, .register_binding = i}};
          if ((sres = dyn_libs::spvc_compiler_hlsl_add_resource_binding(scompiler, &rb)) != SPVC_SUCCESS)
          {
            Error::SetStringFmt(error, "spvc_compiler_hlsl_add_resource_binding() failed: {}", static_cast<int>(sres));
            return {};
          }
        }
      }
    }
    break;
#endif

#ifdef ENABLE_OPENGL
    case GPUShaderLanguage::GLSL:
    case GPUShaderLanguage::GLSLES:
    {
      if ((sres = dyn_libs::spvc_compiler_options_set_uint(soptions, SPVC_COMPILER_OPTION_GLSL_VERSION,
                                                           target_version)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(error, "spvc_compiler_options_set_uint(SPVC_COMPILER_OPTION_GLSL_VERSION) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      const bool is_gles = (target_language == GPUShaderLanguage::GLSLES);
      if ((sres = dyn_libs::spvc_compiler_options_set_bool(soptions, SPVC_COMPILER_OPTION_GLSL_ES, is_gles)) !=
          SPVC_SUCCESS)
      {
        Error::SetStringFmt(error, "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_GLSL_ES) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      const bool enable_420pack = (is_gles ? (target_version >= 310) : (target_version >= 420));
      if ((sres = dyn_libs::spvc_compiler_options_set_bool(soptions, SPVC_COMPILER_OPTION_GLSL_ENABLE_420PACK_EXTENSION,
                                                           enable_420pack)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(
          error, "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_GLSL_ENABLE_420PACK_EXTENSION) failed: {}",
          static_cast<int>(sres));
        return {};
      }
    }
    break;
#endif

#ifdef __APPLE__
    case GPUShaderLanguage::MSL:
    {
      if ((sres = dyn_libs::spvc_compiler_options_set_bool(
             soptions, SPVC_COMPILER_OPTION_MSL_PAD_FRAGMENT_OUTPUT_COMPONENTS, true)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(
          error, "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_MSL_PAD_FRAGMENT_OUTPUT_COMPONENTS) failed: {}",
          static_cast<int>(sres));
        return {};
      }

      if ((sres = dyn_libs::spvc_compiler_options_set_bool(soptions, SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS,
                                                           m_features.framebuffer_fetch)) != SPVC_SUCCESS)
      {
        Error::SetStringFmt(
          error, "spvc_compiler_options_set_bool(SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS) failed: {}",
          static_cast<int>(sres));
        return {};
      }

      if (m_features.framebuffer_fetch &&
          ((sres = dyn_libs::spvc_compiler_options_set_uint(soptions, SPVC_COMPILER_OPTION_MSL_VERSION,
                                                            target_version)) != SPVC_SUCCESS))
      {
        Error::SetStringFmt(error, "spvc_compiler_options_set_uint(SPVC_COMPILER_OPTION_MSL_VERSION) failed: {}",
                            static_cast<int>(sres));
        return {};
      }

      if (stage == GPUShaderStage::Fragment)
      {
        for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
        {
          const spvc_msl_resource_binding rb = {.stage = SpvExecutionModelFragment,
                                                .desc_set = 1,
                                                .binding = i,
                                                .msl_buffer = i,
                                                .msl_texture = i,
                                                .msl_sampler = i};

          if ((sres = dyn_libs::spvc_compiler_msl_add_resource_binding(scompiler, &rb)) != SPVC_SUCCESS)
          {
            Error::SetStringFmt(error, "spvc_compiler_msl_add_resource_binding() failed: {}", static_cast<int>(sres));
            return {};
          }
        }

        if (!m_features.framebuffer_fetch)
        {
          const spvc_msl_resource_binding rb = {
            .stage = SpvExecutionModelFragment, .desc_set = 2, .binding = 0, .msl_texture = MAX_TEXTURE_SAMPLERS};

          if ((sres = dyn_libs::spvc_compiler_msl_add_resource_binding(scompiler, &rb)) != SPVC_SUCCESS)
          {
            Error::SetStringFmt(error, "spvc_compiler_msl_add_resource_binding() for FB failed: {}",
                                static_cast<int>(sres));
            return {};
          }
        }
      }
    }
    break;
#endif

    default:
      Error::SetStringFmt(error, "Unsupported target language {}.", ShaderLanguageToString(target_language));
      break;
  }

  if ((sres = dyn_libs::spvc_compiler_install_compiler_options(scompiler, soptions)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_install_compiler_options() failed: {}", static_cast<int>(sres));
    return false;
  }

  const char* out_src;
  if ((sres = dyn_libs::spvc_compiler_compile(scompiler, &out_src)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_compile() failed: {}", static_cast<int>(sres));
    return false;
  }

  const size_t out_src_length = out_src ? std::strlen(out_src) : 0;
  if (out_src_length == 0)
  {
    Error::SetStringView(error, "Failed to compile SPIR-V to target language.");
    return false;
  }

  output->assign(out_src, out_src_length);
  return true;
}

std::unique_ptr<GPUShader> GPUDevice::TranspileAndCreateShaderFromSource(
  GPUShaderStage stage, GPUShaderLanguage source_language, std::string_view source, const char* entry_point,
  GPUShaderLanguage target_language, u32 target_version, DynamicHeapArray<u8>* out_binary, Error* error)
{
  // Currently, entry points must be "main". TODO: rename the entry point in the SPIR-V.
  if (std::strcmp(entry_point, "main") != 0)
  {
    Error::SetStringView(error, "Entry point must be main.");
    return {};
  }

  // Disable optimization when targeting OpenGL GLSL, otherwise, the name-based linking will fail.
  const bool optimization =
    (!m_debug_device && target_language != GPUShaderLanguage::GLSL && target_language != GPUShaderLanguage::GLSLES);

  std::span<const u8> spv;
  DynamicHeapArray<u8> intermediate_spv;
  if (source_language == GPUShaderLanguage::GLSLVK)
  {
    if (!CompileGLSLShaderToVulkanSpv(stage, source_language, source, entry_point, optimization, false,
                                      &intermediate_spv, error))
    {
      return {};
    }

    spv = intermediate_spv.cspan();
  }
  else if (source_language == GPUShaderLanguage::SPV)
  {
    spv = std::span<const u8>(reinterpret_cast<const u8*>(source.data()), source.size());

    if (optimization)
    {
      Error optimize_error;
      std::optional<DynamicHeapArray<u8>> optimized_spv = GPUDevice::OptimizeVulkanSpv(spv, &optimize_error);
      if (!optimized_spv.has_value())
      {
        WARNING_LOG("Failed to optimize SPIR-V: {}", optimize_error.GetDescription());
      }
      else
      {
        DEV_LOG("SPIR-V optimized from {} bytes to {} bytes", source.length(), optimized_spv->size());
        intermediate_spv = std::move(optimized_spv.value());
        spv = intermediate_spv.cspan();
      }
    }
  }
  else
  {
    Error::SetStringFmt(error, "Unsupported source language for transpile: {}",
                        ShaderLanguageToString(source_language));
    return {};
  }

  std::string dest_source;
  if (!TranslateVulkanSpvToLanguage(spv, stage, target_language, target_version, &dest_source, error))
    return {};

#ifdef __APPLE__
  // MSL converter suffixes 0.
  if (target_language == GPUShaderLanguage::MSL)
  {
    return CreateShaderFromSource(stage, target_language, dest_source,
                                  TinyString::from_format("{}0", entry_point).c_str(), out_binary, error);
  }
#endif

  return CreateShaderFromSource(stage, target_language, dest_source, entry_point, out_binary, error);
}
