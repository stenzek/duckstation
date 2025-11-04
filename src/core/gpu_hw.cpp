// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_hw.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "fullscreenui_widgets.h"
#include "gpu.h"
#include "gpu_hw_shadergen.h"
#include "gpu_presenter.h"
#include "gpu_sw_rasterizer.h"
#include "gpu_thread.h"
#include "gte_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "system_private.h"

#include "util/imgui_manager.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"
#include "fmt/format.h"
#include "imgui.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

LOG_CHANNEL(GPU_HW);

// TODO: instead of full state restore, only restore what changed

static constexpr GPUTexture::Format VRAM_RT_FORMAT = GPUTexture::Format::RGBA8;
static constexpr GPUTexture::Format VRAM_DS_FORMAT = GPUTexture::Format::D16;
static constexpr GPUTexture::Format VRAM_DS_DEPTH_FORMAT = GPUTexture::Format::D32F;
static constexpr GPUTexture::Format VRAM_DS_COLOR_FORMAT = GPUTexture::Format::R32F;

#if defined(_DEBUG) || defined(_DEVEL)

static u32 s_draw_number = 0;

static constexpr const std::array s_transparency_modes = {
  "HalfBackgroundPlusHalfForeground",
  "BackgroundPlusForeground",
  "BackgroundMinusForeground",
  "BackgroundPlusQuarterForeground",
  "Disabled",
};

static constexpr const std::array s_batch_texture_modes = {
  "Palette4Bit",       "Palette8Bit",       "Direct16Bit",       "PageTexture",       "Disabled",
  "SpritePalette4Bit", "SpritePalette8Bit", "SpriteDirect16Bit", "SpritePageTexture",
};
static_assert(s_batch_texture_modes.size() == static_cast<size_t>(GPU_HW::BatchTextureMode::MaxCount));

static constexpr const std::array s_batch_render_modes = {
  "TransparencyDisabled", "TransparentAndOpaque", "OnlyOpaque", "OnlyTransparent", "ShaderBlend",
};
static_assert(s_batch_render_modes.size() == static_cast<size_t>(GPU_HW::BatchRenderMode::MaxCount));

#endif

/// Returns the distance between two rectangles.
ALWAYS_INLINE static float RectDistance(const GSVector4i lhs, const GSVector4i rhs)
{
  const s32 lcx = (lhs.left + ((lhs.right - lhs.left) / 2));
  const s32 lcy = (lhs.top + ((lhs.bottom - lhs.top) / 2));
  const s32 rcx = (rhs.left + ((rhs.right - rhs.left) / 2));
  const s32 rcy = (rhs.top + ((rhs.bottom - rhs.top) / 2));
  const s32 dx = (lcx - rcx);
  const s32 dy = (lcy - rcy);
  const s32 distsq = (dx * dx) + (dy * dy);
  return std::sqrt(static_cast<float>(distsq));
}

ALWAYS_INLINE static u32 GetMaxResolutionScale()
{
  return g_gpu_device->GetMaxTextureSize() / VRAM_WIDTH;
}

ALWAYS_INLINE_RELEASE static u32 GetBoxDownsampleScale(u32 resolution_scale)
{
  u32 scale = std::min<u32>(resolution_scale, g_gpu_settings.gpu_downsample_scale);
  while ((resolution_scale % scale) != 0)
    scale--;
  return scale;
}

ALWAYS_INLINE static bool ShouldClampUVs(GPUTextureFilter texture_filter)
{
  // We only need UV limits if PGXP is enabled, or texture filtering is enabled.
  return g_gpu_settings.gpu_pgxp_enable || texture_filter != GPUTextureFilter::Nearest;
}

ALWAYS_INLINE static bool ShouldAllowSpriteMode(u8 resolution_scale, GPUTextureFilter texture_filter,
                                                GPUTextureFilter sprite_texture_filter)
{
  // Use sprite shaders/mode when texcoord rounding is forced, or if the filters are different.
  return (sprite_texture_filter != texture_filter ||
          (resolution_scale > 1 && g_gpu_settings.gpu_force_round_texcoords));
}

ALWAYS_INLINE static bool ShouldDisableColorPerspective()
{
  return g_gpu_settings.gpu_pgxp_enable && g_gpu_settings.gpu_pgxp_texture_correction &&
         !g_gpu_settings.gpu_pgxp_color_correction;
}

/// Returns true if the specified texture filtering mode requires dual-source blending.
ALWAYS_INLINE static bool IsBlendedTextureFiltering(GPUTextureFilter filter)
{
  // return (filter == GPUTextureFilter::Bilinear || filter == GPUTextureFilter::JINC2 || filter ==
  // GPUTextureFilter::xBR);
  static_assert(((static_cast<u8>(GPUTextureFilter::Nearest) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::Bilinear) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::BilinearBinAlpha) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::JINC2) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::JINC2BinAlpha) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::xBR) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::xBRBinAlpha) & 1u) == 0u));
  return (filter < GPUTextureFilter::Scale2x && ((static_cast<u8>(filter) & 1u) == 1u));
}

/// Computes the area affected by a VRAM transfer, including wrap-around of X.
ALWAYS_INLINE_RELEASE static GSVector4i GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height)
{
  GSVector4i ret;
  ret.left = x % VRAM_WIDTH;
  ret.top = y % VRAM_HEIGHT;
  ret.right = ret.left + width;
  ret.bottom = ret.top + height;
  if (ret.right > static_cast<s32>(VRAM_WIDTH))
  {
    ret.left = 0;
    ret.right = static_cast<s32>(VRAM_WIDTH);
  }
  if (ret.bottom > static_cast<s32>(VRAM_HEIGHT))
  {
    ret.top = 0;
    ret.bottom = static_cast<s32>(VRAM_HEIGHT);
  }
  return ret;
}

/// Returns true if the below function should be applied.
ALWAYS_INLINE static bool ShouldTruncate32To16(const GPUBackendDrawCommand* cmd)
{
  return (!cmd->texture_enable && !cmd->shading_enable && !cmd->dither_enable &&
          g_gpu_settings.gpu_dithering_mode == GPUDitheringMode::TrueColor);
}

/// Truncates a 32-bit colour to 16-bit.
ALWAYS_INLINE static u32 Truncate32To16(u32 color)
{
  return GSVector4i((GSVector4(GSVector4i::zext32(color).u8to32().srl32<3>()) / GSVector4::cxpr(31.0f)) *
                    GSVector4::cxpr(255.0f))
    .rgba32();
}

/// Computes the clamped average Z for the given polygon Z values.
template<typename... Args>
ALWAYS_INLINE static float ComputePolygonAverageZ(Args... args)
{
  static_assert(sizeof...(args) >= 2, "At least two arguments are required");
  const float sum = (args + ...);
  constexpr s32 count = static_cast<s32>(sizeof...(args));
  return std::min(sum / static_cast<float>(count), 1.0f);
}

namespace {
class ShaderCompileProgressTracker
{
public:
  ShaderCompileProgressTracker(u32 total)
    : m_image(System::GetImageForLoadingScreen(GPUThread::GetGamePath())),
      m_min_time(Timer::ConvertSecondsToValue(1.0)), m_update_interval(Timer::ConvertSecondsToValue(0.1)),
      m_start_time(Timer::GetCurrentValue()), m_last_update_time(0), m_progress(0), m_total(total)
  {
  }
  ~ShaderCompileProgressTracker() = default;

  double GetElapsedMilliseconds() const
  {
    return Timer::ConvertValueToMilliseconds(Timer::GetCurrentValue() - m_start_time);
  }

  bool Increment(u32 progress, Error* error)
  {
    m_progress += progress;

    if (System::IsStartupCancelled())
    {
      Error::SetStringView(error, TRANSLATE_SV("System", "Startup was cancelled."));
      ERROR_LOG("Shader compilation aborted due to cancelled startup");
      return false;
    }

    const u64 tv = Timer::GetCurrentValue();
    if ((tv - m_start_time) >= m_min_time && (tv - m_last_update_time) >= m_update_interval)
    {
      FullscreenUI::RenderLoadingScreen(
        m_image, TRANSLATE_SV("GPU_HW", "Compiling Shaders..."),
        SmallString::from_format(TRANSLATE_FS("GPU_HW", "{} of {} pipelines"), m_progress, m_total), 0,
        static_cast<int>(m_total), static_cast<int>(m_progress));
      m_last_update_time = tv;
    }

    return true;
  }

private:
  std::string m_image;
  Timer::Value m_min_time;
  Timer::Value m_update_interval;
  Timer::Value m_start_time;
  Timer::Value m_last_update_time;
  u32 m_progress;
  u32 m_total;
};
} // namespace

GPU_HW::GPU_HW(GPUPresenter& presenter) : GPUBackend(presenter)
{
#if defined(_DEBUG) || defined(_DEVEL)
  s_draw_number = 0;
#endif
}

GPU_HW::~GPU_HW()
{
  GPUTextureCache::Shutdown();
}

ALWAYS_INLINE void GPU_HW::BatchVertex::Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_,
                                            u16 packed_texcoord, u32 uv_limits_)
{
  Set(x_, y_, z_, w_, color_, texpage_, packed_texcoord & 0xFF, (packed_texcoord >> 8), uv_limits_);
}

ALWAYS_INLINE void GPU_HW::BatchVertex::Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 u_,
                                            u16 v_, u32 uv_limits_)
{
  x = x_;
  y = y_;
  z = z_;
  w = w_;
  color = color_;
  texpage = texpage_;
  u = u_;
  v = v_;
  uv_limits = uv_limits_;
}

ALWAYS_INLINE u32 GPU_HW::BatchVertex::PackUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
{
  return min_u | (min_v << 8) | (max_u << 16) | (max_v << 24);
}

ALWAYS_INLINE void GPU_HW::BatchVertex::SetUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
{
  uv_limits = PackUVLimits(min_u, max_u, min_v, max_v);
}

bool GPU_HW::Initialize(bool upload_vram, Error* error)
{
  if (!GPUBackend::Initialize(upload_vram, error))
    return false;

  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  m_resolution_scale = Truncate8(CalculateResolutionScale());
  m_multisamples = Truncate8(std::min<u32>(g_gpu_settings.gpu_multisamples, g_gpu_device->GetMaxMultisamples()));
  m_texture_filtering = g_gpu_settings.gpu_texture_filter;
  m_sprite_texture_filtering = g_gpu_settings.gpu_sprite_texture_filter;
  m_line_detect_mode = (m_resolution_scale > 1) ? g_gpu_settings.gpu_line_detect_mode : GPULineDetectMode::Disabled;
  m_downsample_mode = GetDownsampleMode(m_resolution_scale);
  m_wireframe_mode = g_gpu_settings.gpu_wireframe_mode;
  m_supports_dual_source_blend = features.dual_source_blend;
  m_supports_framebuffer_fetch = features.framebuffer_fetch;
  m_true_color = g_gpu_settings.IsUsingTrueColor();
  m_pgxp_depth_buffer = g_gpu_settings.UsingPGXPDepthBuffer();
  m_clamp_uvs = ShouldClampUVs(m_texture_filtering) || ShouldClampUVs(m_sprite_texture_filtering);
  m_compute_uv_range = m_clamp_uvs;
  m_allow_sprite_mode = ShouldAllowSpriteMode(m_resolution_scale, m_texture_filtering, m_sprite_texture_filtering);
  m_use_texture_cache = g_gpu_settings.gpu_texture_cache;
  m_texture_dumping = m_use_texture_cache && g_gpu_settings.texture_replacements.dump_textures;

  CheckSettings();

  PrintSettingsToLog();

  if (!CompileCommonShaders(error) || !CompilePipelines(error) || !CreateBuffers(error))
    return false;

  if (m_use_texture_cache)
  {
    if (!GPUTextureCache::Initialize(this, error))
      return false;
  }
  else
  {
    // Still potentially have VRAM texture replacements.
    GPUTextureCache::ReloadTextureReplacements(System::GetState() == System::State::Starting, false);
  }

  UpdateDownsamplingLevels();

  RestoreDeviceContext();

  // If we're not initializing VRAM, need to upload it here. Implies RestoreDeviceContext().
  if (upload_vram)
    UpdateVRAMOnGPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT, g_vram, VRAM_WIDTH * sizeof(u16), false, false, VRAM_SIZE_RECT);

  m_drawing_area_changed = true;
  LoadInternalPostProcessing();
  return true;
}

u32 GPU_HW::GetResolutionScale() const
{
  return m_resolution_scale;
}

void GPU_HW::ClearVRAM()
{
  // Texture cache needs to be invalidated before we load, otherwise we dump black.
  if (m_use_texture_cache)
    GPUTextureCache::Invalidate();

  // Don't need to finish the current draw.
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(0, 0);

  m_texpage_dirty = false;
  m_compute_uv_range = m_clamp_uvs;

  std::memset(g_vram, 0, sizeof(g_vram));
  std::memset(g_gpu_clut, 0, sizeof(g_gpu_clut));

  m_batch = {};
  m_current_depth = 1;
  ClearFramebuffer();
}

void GPU_HW::LoadState(const GPUBackendLoadStateCommand* cmd)
{
  DebugAssert((m_batch_vertex_ptr != nullptr) == (m_batch_index_ptr != nullptr));
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(0, 0);

  std::memcpy(g_vram, cmd->vram_data, sizeof(g_vram));
  std::memcpy(g_gpu_clut, cmd->clut_data, sizeof(g_gpu_clut));
  UpdateVRAMOnGPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT, g_vram, VRAM_WIDTH * sizeof(u16), false, false, VRAM_SIZE_RECT);

  if (m_use_texture_cache)
  {
    StateWrapper sw(std::span<const u8>(cmd->texture_cache_state, cmd->texture_cache_state_size),
                    StateWrapper::Mode::Read, cmd->texture_cache_state_version);
    if (!GPUTextureCache::DoState(sw, false)) [[unlikely]]
      Panic("Failed to process texture cache state.");
  }

  m_batch = {};
  m_current_depth = 1;
  ClearVRAMDirtyRectangle();
  SetFullVRAMDirtyRectangle();
  UpdateVRAMReadTexture(true, false);
  ClearVRAMDirtyRectangle();
  ResetBatchVertexDepth();
}

bool GPU_HW::AllocateMemorySaveState(System::MemorySaveState& mss, Error* error)
{
  mss.vram_texture = g_gpu_device->FetchTexture(
    m_vram_texture->GetWidth(), m_vram_texture->GetHeight(), 1, 1, m_vram_texture->GetSamples(),
    m_vram_texture->IsMultisampled() ? GPUTexture::Type::RenderTarget : GPUTexture::Type::Texture,
    GPUTexture::Format::RGBA8, GPUTexture::Flags::None, nullptr, 0, error);
  if (!mss.vram_texture) [[unlikely]]
  {
    Error::AddPrefix(error, "Failed to allocate VRAM texture for memory save state: ");
    return false;
  }

  GL_OBJECT_NAME(mss.vram_texture, "Memory save state VRAM copy");

  static constexpr u32 MAX_TC_SIZE = 1024 * 1024;

  u32 buffer_size = 0;
  if (ShouldDrawWithSoftwareRenderer() || m_use_texture_cache)
    buffer_size += sizeof(g_vram);
  if (ShouldDrawWithSoftwareRenderer())
    buffer_size += sizeof(g_gpu_clut);
  if (m_use_texture_cache)
    buffer_size += MAX_TC_SIZE;

  if (buffer_size > 0)
    mss.gpu_state_data.resize(buffer_size);

  return true;
}

void GPU_HW::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss)
{
  Assert(mss.vram_texture && mss.vram_texture->GetWidth() == m_vram_texture->GetWidth() &&
         mss.vram_texture->GetHeight() == m_vram_texture->GetHeight() &&
         mss.vram_texture->GetSamples() == m_vram_texture->GetSamples());

  if (sw.IsReading())
  {
    if (m_batch_vertex_ptr)
      UnmapGPUBuffer(0, 0);

    g_gpu_device->CopyTextureRegion(m_vram_texture.get(), 0, 0, 0, 0, mss.vram_texture.get(), 0, 0, 0, 0,
                                    m_vram_texture->GetWidth(), m_vram_texture->GetHeight());

    m_batch = {};
    ClearVRAMDirtyRectangle();
    SetFullVRAMDirtyRectangle();
    UpdateVRAMReadTexture(true, false);
    ClearVRAMDirtyRectangle();
    ResetBatchVertexDepth();
  }
  else
  {
    FlushRender();

    // saving state
    g_gpu_device->CopyTextureRegion(mss.vram_texture.get(), 0, 0, 0, 0, m_vram_texture.get(), 0, 0, 0, 0,
                                    m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
  }

  // Save VRAM/CLUT.
  if (ShouldDrawWithSoftwareRenderer() || m_use_texture_cache)
    sw.DoBytes(g_vram, sizeof(g_vram));
  if (ShouldDrawWithSoftwareRenderer())
    sw.DoBytes(g_gpu_clut, sizeof(g_gpu_clut));
  if (m_use_texture_cache)
  {
    if (!GPUTextureCache::DoState(sw, false)) [[unlikely]]
      Panic("Failed to process texture cache state.");
  }
}

void GPU_HW::RestoreDeviceContext()
{
  g_gpu_device->SetTextureSampler(0, m_vram_read_texture.get(), g_gpu_device->GetNearestSampler());
  SetVRAMRenderTarget();
  g_gpu_device->SetViewport(m_vram_texture->GetRect());
  SetScissor();
  m_batch_ubo_dirty = true;
}

bool GPU_HW::UpdateSettings(const GPUSettings& old_settings, Error* error)
{
  if (!GPUBackend::UpdateSettings(old_settings, error))
    return false;

  FlushRender();

  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  const u8 resolution_scale = Truncate8(CalculateResolutionScale());
  const u8 multisamples = Truncate8(std::min<u32>(g_gpu_settings.gpu_multisamples, g_gpu_device->GetMaxMultisamples()));
  const bool clamp_uvs = ShouldClampUVs(m_texture_filtering) || ShouldClampUVs(m_sprite_texture_filtering);
  const bool framebuffer_changed = (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
                                    g_gpu_settings.IsUsingShaderBlending() != old_settings.IsUsingShaderBlending() ||
                                    m_pgxp_depth_buffer != g_gpu_settings.UsingPGXPDepthBuffer() ||
                                    (!old_settings.gpu_texture_cache && g_gpu_settings.gpu_texture_cache));
  const bool shaders_changed =
    ((m_resolution_scale > 1) != (resolution_scale > 1) || m_multisamples != multisamples ||
     m_true_color != g_gpu_settings.IsUsingTrueColor() ||
     (old_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive) !=
       (g_gpu_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive) ||
     (multisamples > 1 && g_gpu_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading) ||
     (resolution_scale > 1 && (g_gpu_settings.IsUsingScaledDithering() != old_settings.IsUsingScaledDithering() ||
                               g_gpu_settings.gpu_scaled_interlacing != old_settings.gpu_scaled_interlacing)) ||
     (resolution_scale > 1 && g_gpu_settings.gpu_texture_filter == GPUTextureFilter::Nearest &&
      g_gpu_settings.gpu_force_round_texcoords != old_settings.gpu_force_round_texcoords) ||
     g_gpu_settings.IsUsingShaderBlending() != old_settings.IsUsingShaderBlending() ||
     m_texture_filtering != g_gpu_settings.gpu_texture_filter ||
     m_sprite_texture_filtering != g_gpu_settings.gpu_sprite_texture_filter || m_clamp_uvs != clamp_uvs ||
     (features.geometry_shaders && g_gpu_settings.gpu_wireframe_mode != old_settings.gpu_wireframe_mode) ||
     m_pgxp_depth_buffer != g_gpu_settings.UsingPGXPDepthBuffer() ||
     (features.noperspective_interpolation && g_gpu_settings.gpu_pgxp_enable &&
      g_gpu_settings.gpu_pgxp_color_correction != old_settings.gpu_pgxp_color_correction) ||
     m_allow_sprite_mode != ShouldAllowSpriteMode(m_resolution_scale, g_gpu_settings.gpu_texture_filter,
                                                  g_gpu_settings.gpu_sprite_texture_filter) ||
     (!old_settings.gpu_texture_cache && g_gpu_settings.gpu_texture_cache));
  const bool resolution_dependent_shaders_changed =
    (m_resolution_scale != resolution_scale || m_multisamples != multisamples);
  const bool downsampling_shaders_changed =
    ((m_resolution_scale > 1) != (resolution_scale > 1) ||
     (resolution_scale > 1 && (g_gpu_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
                               (g_gpu_settings.gpu_downsample_mode == GPUDownsampleMode::Box &&
                                (resolution_scale != m_resolution_scale ||
                                 g_gpu_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale)))));

  if (m_resolution_scale != resolution_scale)
  {
    Host::AddIconOSDMessage("ResolutionScaleChanged", ICON_FA_PAINTBRUSH,
                            fmt::format(TRANSLATE_FS("GPU_HW", "Internal resolution set to {0}x ({1}x{2})."),
                                        resolution_scale, m_presenter.GetDisplayWidth() * resolution_scale,
                                        m_presenter.GetDisplayHeight() * resolution_scale),
                            Host::OSD_INFO_DURATION);
  }

  if (m_multisamples != multisamples || g_gpu_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading)
  {
    if (g_gpu_settings.gpu_per_sample_shading && features.per_sample_shading)
    {
      Host::AddIconOSDMessage(
        "MultisamplingChanged", ICON_FA_PAINTBRUSH,
        fmt::format(TRANSLATE_FS("GPU_HW", "Multisample anti-aliasing set to {}x (SSAA)."), multisamples),
        Host::OSD_INFO_DURATION);
    }
    else
    {
      Host::AddIconOSDMessage(
        "MultisamplingChanged", ICON_FA_PAINTBRUSH,
        fmt::format(TRANSLATE_FS("GPU_HW", "Multisample anti-aliasing set to {}x."), multisamples),
        Host::OSD_INFO_DURATION);
    }
  }

  // Back up VRAM if we're recreating the framebuffer.
  if (framebuffer_changed)
  {
    RestoreDeviceContext();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    DestroyBuffers();
  }

  m_resolution_scale = resolution_scale;
  m_multisamples = multisamples;
  m_texture_filtering = g_gpu_settings.gpu_texture_filter;
  m_sprite_texture_filtering = g_gpu_settings.gpu_sprite_texture_filter;
  m_line_detect_mode = (m_resolution_scale > 1) ? g_gpu_settings.gpu_line_detect_mode : GPULineDetectMode::Disabled;
  m_downsample_mode = GetDownsampleMode(resolution_scale);
  m_wireframe_mode = g_gpu_settings.gpu_wireframe_mode;
  m_true_color = g_gpu_settings.IsUsingTrueColor();
  m_clamp_uvs = clamp_uvs;
  m_compute_uv_range = m_clamp_uvs;
  m_allow_sprite_mode = ShouldAllowSpriteMode(resolution_scale, m_texture_filtering, m_sprite_texture_filtering);
  m_use_texture_cache = g_gpu_settings.gpu_texture_cache;
  m_texture_dumping = m_use_texture_cache && g_gpu_settings.texture_replacements.dump_textures;
  m_batch.sprite_mode = (m_allow_sprite_mode && m_batch.sprite_mode);

  const bool depth_buffer_changed = (m_pgxp_depth_buffer != g_gpu_settings.UsingPGXPDepthBuffer());
  if (depth_buffer_changed)
  {
    m_pgxp_depth_buffer = g_gpu_settings.UsingPGXPDepthBuffer();
    m_batch.use_depth_buffer = false;
    m_depth_was_copied = false;
  }

  CheckSettings();

  PrintSettingsToLog();

  if (shaders_changed)
  {
    if (!CompilePipelines(error))
    {
      Error::AddPrefix(error, "Failed to recompile pipelines: ");
      return false;
    }
  }
  else if (resolution_dependent_shaders_changed || downsampling_shaders_changed)
  {
    if ((resolution_dependent_shaders_changed && !CompileResolutionDependentPipelines(error)) ||
        (downsampling_shaders_changed && !CompileDownsamplePipelines(error)))
    {
      Error::AddPrefix(error, "Failed to recompile resolution dependent pipelines: ");
      return false;
    }
  }

  if (framebuffer_changed)
  {
    // When using very high upscaling, it's possible that we don't have enough VRAM for two sets of buffers.
    // Purge the pool, and idle the GPU so that all video memory is freed prior to creating the new buffers.
    g_gpu_device->PurgeTexturePool();
    g_gpu_device->WaitForGPUIdle();

    if (!CreateBuffers(error))
    {
      Error::AddPrefix(error, "Failed to recreate buffers: ");
      return false;
    }

    UpdateDownsamplingLevels();
    RestoreDeviceContext();
    UpdateVRAMOnGPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT, g_vram, VRAM_WIDTH * sizeof(u16), false, false, VRAM_SIZE_RECT);
    if (m_write_mask_as_depth)
      UpdateDepthBufferFromMaskBit();
  }
  else if (m_vram_depth_texture && depth_buffer_changed)
  {
    if (m_pgxp_depth_buffer)
      ClearDepthBuffer(false);
    else if (m_write_mask_as_depth)
      UpdateDepthBufferFromMaskBit();
  }

  if (m_use_texture_cache && !old_settings.gpu_texture_cache)
  {
    if (!GPUTextureCache::Initialize(this, error))
    {
      Error::AddPrefix(error, "Failed to initialize texture cache: ");
      return false;
    }
  }
  else if (!m_use_texture_cache && old_settings.gpu_texture_cache)
  {
    GPUTextureCache::Shutdown();
  }

  if (!GPUTextureCache::UpdateSettings(m_use_texture_cache, old_settings, error))
    return false;

  if (g_gpu_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
      (g_gpu_settings.gpu_downsample_mode == GPUDownsampleMode::Box &&
       g_gpu_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale))
  {
    UpdateDownsamplingLevels();
  }

  // Need to reload CLUT if we're enabling SW rendering.
  if (g_gpu_settings.gpu_use_software_renderer_for_readbacks && !old_settings.gpu_use_software_renderer_for_readbacks)
  {
    DownloadVRAMFromGPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

    if (m_draw_mode.mode_reg.texture_mode <= GPUTextureMode::Palette8Bit)
    {
      GPU_SW_Rasterizer::UpdateCLUT(m_draw_mode.palette_reg,
                                    m_draw_mode.mode_reg.texture_mode == GPUTextureMode::Palette8Bit);
    }
  }

  return true;
}

void GPU_HW::CheckSettings()
{
  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  if (m_multisamples != g_gpu_settings.gpu_multisamples)
  {
    Host::AddIconOSDMessage("MSAAUnsupported", ICON_EMOJI_WARNING,
                            fmt::format(TRANSLATE_FS("GPU_HW", "{}x MSAA is not supported, using {}x instead."),
                                        g_gpu_settings.gpu_multisamples, m_multisamples),
                            Host::OSD_CRITICAL_ERROR_DURATION);
  }
  else
  {
    Host::RemoveKeyedOSDMessage("MSAAUnsupported");
  }

  if (g_gpu_settings.gpu_per_sample_shading && !features.per_sample_shading)
  {
    Host::AddIconOSDMessage("SSAAUnsupported", ICON_EMOJI_WARNING,
                            TRANSLATE_STR("GPU_HW", "SSAA is not supported, using MSAA instead."),
                            Host::OSD_ERROR_DURATION);
  }
  if (!features.dual_source_blend && !features.framebuffer_fetch &&
      (IsBlendedTextureFiltering(m_texture_filtering) || IsBlendedTextureFiltering(m_sprite_texture_filtering)))
  {
    Host::AddIconOSDMessage(
      "TextureFilterUnsupported", ICON_EMOJI_WARNING,
      fmt::format(TRANSLATE_FS("GPU_HW", "Texture filter '{}/{}' is not supported with the current renderer."),
                  Settings::GetTextureFilterDisplayName(m_texture_filtering),
                  Settings::GetTextureFilterName(m_sprite_texture_filtering), Host::OSD_ERROR_DURATION));
    m_texture_filtering = GPUTextureFilter::Nearest;
    m_sprite_texture_filtering = GPUTextureFilter::Nearest;
    m_allow_sprite_mode = ShouldAllowSpriteMode(m_resolution_scale, m_texture_filtering, m_sprite_texture_filtering);
  }

  if (g_gpu_settings.IsUsingShaderBlending() && !m_supports_framebuffer_fetch && !features.feedback_loops &&
      !features.raster_order_views)
  {
    // m_allow_shader_blend/m_prefer_shader_blend will be cleared in pipeline compile.
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GPU_HW", "Shader blending is not supported by your current GPU."),
      TRANSLATE_STR("GPU_HW", "It requires framebuffer fetch, feedback loops, or rasterizer order views."),
      Host::OSD_WARNING_DURATION);
  }
  else if (IsUsingMultisampling() && !features.framebuffer_fetch &&
           ((g_gpu_settings.IsUsingShaderBlending() && features.raster_order_views) ||
            (m_pgxp_depth_buffer && features.raster_order_views && !features.feedback_loops)))
  {
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GPU_HW", "Multisample anti-aliasing is not supported when using shader blending."),
      Host::OSD_WARNING_DURATION);
    m_multisamples = 1;
  }

  if (m_pgxp_depth_buffer && !features.feedback_loops && !features.framebuffer_fetch && !features.raster_order_views)
  {
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GPU_HW", "PGXP depth buffer is not supported by your current GPU or renderer."),
      TRANSLATE_STR("GPU_HW", "It requires framebuffer fetch, feedback loops, or rasterizer order views."),
      Host::OSD_WARNING_DURATION);
    m_pgxp_depth_buffer = false;
  }

  if (!features.noperspective_interpolation && !ShouldDisableColorPerspective())
    WARNING_LOG("Disable color perspective not supported, but should be used.");

  if (!features.geometry_shaders && m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    Host::AddIconOSDMessage(
      "GeometryShadersUnsupported", ICON_EMOJI_WARNING,
      TRANSLATE("GPU_HW", "Geometry shaders are not supported by your GPU, and are required for wireframe rendering."),
      Host::OSD_CRITICAL_ERROR_DURATION);
    m_wireframe_mode = GPUWireframeMode::Disabled;
  }

  if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    const u32 resolution_scale = CalculateResolutionScale();
    const u32 box_downscale = GetBoxDownsampleScale(resolution_scale);
    if (box_downscale == resolution_scale)
    {
      m_downsample_mode = GPUDownsampleMode::Disabled;

      Host::AddIconOSDMessage(
        "BoxDownsampleUnsupported", ICON_FA_PAINTBRUSH,
        fmt::format(
          TRANSLATE_FS("GPU_HW",
                       "Resolution scale {0}x is not divisible by downsample scale {1}x, downsampling disabled."),
          resolution_scale, g_gpu_settings.gpu_downsample_scale),
        Host::OSD_WARNING_DURATION);
    }
    else if (box_downscale != g_gpu_settings.gpu_downsample_scale)
    {
      Host::AddIconOSDMessage(
        "BoxDownsampleUnsupported", ICON_FA_PAINTBRUSH,
        fmt::format(TRANSLATE_FS(
                      "GPU_HW", "Resolution scale {0}x is not divisible by downsample scale {1}x, using {2}x instead."),
                    resolution_scale, g_gpu_settings.gpu_downsample_scale, box_downscale),
        Host::OSD_WARNING_DURATION);
    }
    else
    {
      Host::RemoveKeyedOSDMessage("BoxDownsampleUnsupported");
    }
  }
}

u32 GPU_HW::CalculateResolutionScale() const
{
  u32 scale = g_gpu_settings.gpu_resolution_scale;

  if (g_gpu_settings.gpu_downsample_mode == GPUDownsampleMode::Adaptive && scale > 1 && !Common::IsPow2(scale))
  {
    const u32 new_scale = Common::PreviousPow2(scale);
    WARNING_LOG("Resolution scale {}x not supported for adaptive downsampling, using {}x", scale, new_scale);

    if (g_gpu_settings.gpu_resolution_scale != 0)
    {
      Host::AddIconOSDMessage(
        "ResolutionNotPow2", ICON_FA_PAINTBRUSH,
        fmt::format(
          TRANSLATE_FS("GPU_HW", "Resolution scale {0}x not supported for adaptive downsampling, using {1}x."), scale,
          new_scale),
        Host::OSD_WARNING_DURATION);
    }

    scale = new_scale;
  }

  return std::clamp<u32>(scale, 1, GetMaxResolutionScale());
}

GPUDownsampleMode GPU_HW::GetDownsampleMode(u32 resolution_scale) const
{
  return (resolution_scale == 1) ? GPUDownsampleMode::Disabled : g_gpu_settings.gpu_downsample_mode;
}

bool GPU_HW::ShouldDrawWithSoftwareRenderer() const
{
  // TODO: FIXME: Move into class.
  return g_gpu_settings.gpu_use_software_renderer_for_readbacks;
}

bool GPU_HW::IsUsingMultisampling() const
{
  return m_multisamples > 1;
}

bool GPU_HW::IsUsingDownsampling(const GPUBackendUpdateDisplayCommand* cmd) const
{
  return (m_downsample_mode != GPUDownsampleMode::Disabled && !cmd->display_24bit);
}

void GPU_HW::SetFullVRAMDirtyRectangle()
{
  m_vram_dirty_draw_rect = VRAM_SIZE_RECT;
  m_draw_mode.bits = INVALID_DRAW_MODE_BITS;
}

void GPU_HW::ClearVRAMDirtyRectangle()
{
  m_vram_dirty_draw_rect = INVALID_RECT;
  m_vram_dirty_write_rect = INVALID_RECT;
}

void GPU_HW::AddWrittenRectangle(const GSVector4i rect)
{
  m_vram_dirty_write_rect = m_vram_dirty_write_rect.runion(rect);
  SetTexPageChangedOnOverlap(m_vram_dirty_write_rect);

  if (m_use_texture_cache)
    GPUTextureCache::AddWrittenRectangle(rect);
}

void GPU_HW::AddDrawnRectangle(const GSVector4i rect)
{
  // Normally, we would check for overlap here. But the GPU's texture cache won't actually reload until the page
  // changes, or it samples a larger region, so we can get away without doing so. This reduces copies considerably in
  // games like Mega Man Legends 2.
  if (m_current_draw_rect.rcontains(rect))
    return;

  m_current_draw_rect = m_current_draw_rect.runion(rect);
  m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(m_current_draw_rect);

  if (m_use_texture_cache)
    GPUTextureCache::AddDrawnRectangle(m_current_draw_rect, m_clamped_drawing_area);
}

void GPU_HW::AddUnclampedDrawnRectangle(const GSVector4i rect)
{
  m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(rect);
  SetTexPageChangedOnOverlap(m_vram_dirty_draw_rect);
  if (m_use_texture_cache)
    GPUTextureCache::AddDrawnRectangle(rect, rect);
}

void GPU_HW::SetTexPageChangedOnOverlap(const GSVector4i update_rect)
{
  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (m_draw_mode.bits != INVALID_DRAW_MODE_BITS && m_batch.texture_mode != BatchTextureMode::Disabled &&
      (GetTextureRect(m_draw_mode.mode_reg.texture_page, m_draw_mode.mode_reg.texture_mode).rintersects(update_rect) ||
       (m_draw_mode.mode_reg.IsUsingPalette() &&
        GetPaletteRect(m_draw_mode.palette_reg, m_draw_mode.mode_reg.texture_mode).rintersects(update_rect))))
  {
    m_draw_mode.bits = INVALID_DRAW_MODE_BITS;
  }
}

void GPU_HW::PrintSettingsToLog()
{
  INFO_LOG("Resolution Scale: {} ({}x{}), maximum {}", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
           VRAM_HEIGHT * m_resolution_scale, GetMaxResolutionScale());
  INFO_LOG("Multisampling: {}x{}", m_multisamples,
           (g_gpu_settings.gpu_per_sample_shading && g_gpu_device->GetFeatures().per_sample_shading) ?
             " (per sample shading)" :
             "");
  INFO_LOG("Dithering: {}", Settings::GetGPUDitheringModeDisplayName(g_gpu_settings.gpu_dithering_mode));
  INFO_LOG("Deinterlacing: {}{}",
           Settings::GetDisplayDeinterlacingModeDisplayName(g_gpu_settings.display_deinterlacing_mode),
           (m_resolution_scale > 1 && g_gpu_settings.gpu_scaled_interlacing) ? " (scaled)" : "");
  INFO_LOG("Force round texture coordinates: {}",
           (m_resolution_scale > 1 && g_gpu_settings.gpu_force_round_texcoords) ? "Enabled" : "Disabled");
  INFO_LOG("Texture Filtering: {}/{}", Settings::GetTextureFilterDisplayName(m_texture_filtering),
           Settings::GetTextureFilterDisplayName(m_sprite_texture_filtering));
  INFO_LOG("Dual-source blending: {}", m_supports_dual_source_blend ? "Supported" : "Not supported");
  INFO_LOG("Clamping UVs: {}", m_clamp_uvs ? "YES" : "NO");
  INFO_LOG("Depth buffer: {}", m_pgxp_depth_buffer ? "YES" : "NO");
  INFO_LOG("Downsampling: {}", Settings::GetDownsampleModeDisplayName(m_downsample_mode));
  INFO_LOG("Wireframe rendering: {}", Settings::GetGPUWireframeModeDisplayName(m_wireframe_mode));
  INFO_LOG("Line detection: {}", Settings::GetLineDetectModeDisplayName(m_line_detect_mode));
  INFO_LOG("Using software renderer for readbacks: {}", ShouldDrawWithSoftwareRenderer() ? "YES" : "NO");
  INFO_LOG("Separate sprite shaders: {}", m_allow_sprite_mode ? "YES" : "NO");
}

GPUTexture::Format GPU_HW::GetDepthBufferFormat() const
{
  // Use 32-bit depth for PGXP depth buffer, otherwise 16-bit for mask bit.
  return m_pgxp_depth_buffer ? (m_use_rov_for_shader_blend ? VRAM_DS_COLOR_FORMAT : VRAM_DS_DEPTH_FORMAT) :
                               VRAM_DS_FORMAT;
}

bool GPU_HW::CreateBuffers(Error* error)
{
  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const u8 samples = static_cast<u8>(m_multisamples);
  const bool needs_depth_buffer = m_write_mask_as_depth || m_pgxp_depth_buffer;

  const GPUTexture::Flags read_texture_flags =
    (m_multisamples > 1) ? GPUTexture::Flags::AllowMSAAResolveTarget : GPUTexture::Flags::None;
  const GPUTexture::Flags vram_texture_flags =
    m_use_rov_for_shader_blend ? GPUTexture::Flags::AllowBindAsImage : GPUTexture::Flags::None;
  const GPUTexture::Type depth_texture_type =
    m_use_rov_for_shader_blend ? GPUTexture::Type::RenderTarget : GPUTexture::Type::DepthStencil;

  if (!(m_vram_texture =
          g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, samples, GPUTexture::Type::RenderTarget,
                                     VRAM_RT_FORMAT, vram_texture_flags, nullptr, 0, error)) ||
      (needs_depth_buffer && !(m_vram_depth_texture = g_gpu_device->FetchTexture(
                                 texture_width, texture_height, 1, 1, samples, depth_texture_type,
                                 GetDepthBufferFormat(), vram_texture_flags, nullptr, 0, error))) ||
      (m_pgxp_depth_buffer && !(m_vram_depth_copy_texture = g_gpu_device->FetchTexture(
                                  texture_width, texture_height, 1, 1, samples, GPUTexture::Type::RenderTarget,
                                  VRAM_DS_COLOR_FORMAT, GPUTexture::Flags::None, nullptr, 0, error))) ||
      !(m_vram_read_texture =
          g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, 1, GPUTexture::Type::Texture, VRAM_RT_FORMAT,
                                     read_texture_flags, nullptr, 0, error)) ||
      !(m_vram_readback_texture =
          g_gpu_device->FetchTexture(VRAM_WIDTH / 2, VRAM_HEIGHT, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                     VRAM_RT_FORMAT, GPUTexture::Flags::None, nullptr, 0, error)))
  {
    Error::AddPrefix(error, "Failed to create VRAM textures: ");
    return false;
  }

  GL_OBJECT_NAME(m_vram_texture, "VRAM Texture");
  if (m_vram_depth_texture)
    GL_OBJECT_NAME(m_vram_depth_texture, "VRAM Depth Texture");
  GL_OBJECT_NAME(m_vram_read_texture, "VRAM Read Texture");
  GL_OBJECT_NAME(m_vram_readback_texture, "VRAM Readback Texture");

  if (g_gpu_device->GetFeatures().memory_import)
  {
    DEV_LOG("Trying to import guest VRAM buffer for downloads...");
    m_vram_readback_download_texture = g_gpu_device->CreateDownloadTexture(
      m_vram_readback_texture->GetWidth(), m_vram_readback_texture->GetHeight(), m_vram_readback_texture->GetFormat(),
      g_vram, sizeof(g_vram), VRAM_WIDTH * sizeof(u16), error);
    if (!m_vram_readback_download_texture)
      ERROR_LOG("Failed to create imported readback buffer");
  }
  if (!m_vram_readback_download_texture)
  {
    m_vram_readback_download_texture =
      g_gpu_device->CreateDownloadTexture(m_vram_readback_texture->GetWidth(), m_vram_readback_texture->GetHeight(),
                                          m_vram_readback_texture->GetFormat(), error);
    if (!m_vram_readback_download_texture)
    {
      Error::AddPrefix(error, "Failed to create readback download texture: ");
      return false;
    }
  }

  if (g_gpu_device->GetFeatures().texture_buffers)
  {
    if (!(m_vram_upload_buffer = g_gpu_device->CreateTextureBuffer(GPUTextureBuffer::Format::R16UI,
                                                                   GPUDevice::MIN_TEXEL_BUFFER_ELEMENTS, error)))
    {
      Error::AddPrefix(error, "Failed to create texture buffer: ");
      return false;
    }

    GL_OBJECT_NAME(m_vram_upload_buffer, "VRAM Upload Buffer");
  }

  INFO_LOG("Created HW framebuffer of {}x{}", texture_width, texture_height);

  m_batch_ubo_data.u_resolution_scale = static_cast<float>(m_resolution_scale);
  m_batch_ubo_data.u_rcp_resolution_scale = 1.0f / m_batch_ubo_data.u_resolution_scale;
  m_batch_ubo_data.u_resolution_scale_minus_one = m_batch_ubo_data.u_resolution_scale - 1.0f;
  m_batch_ubo_dirty = true;

  SetVRAMRenderTarget();
  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW::ClearFramebuffer()
{
  g_gpu_device->ClearRenderTarget(m_vram_texture.get(), 0);
  if (m_vram_depth_texture)
  {
    if (m_use_rov_for_shader_blend)
      g_gpu_device->ClearRenderTarget(m_vram_depth_texture.get(), 0xFF);
    else
      g_gpu_device->ClearDepth(m_vram_depth_texture.get(), m_pgxp_depth_buffer ? 1.0f : 0.0f);
  }
  ClearVRAMDirtyRectangle();
  if (m_use_texture_cache)
    GPUTextureCache::Invalidate();
  m_last_depth_z = 1.0f;
  m_current_depth = 1;
}

void GPU_HW::SetVRAMRenderTarget()
{
  if (m_use_rov_for_shader_blend)
  {
    GPUTexture* rts[2] = {m_vram_texture.get(), m_vram_depth_texture.get()};
    const u32 num_rts = m_pgxp_depth_buffer ? 2 : 1;
    g_gpu_device->SetRenderTargets(
      rts, num_rts, nullptr, m_rov_active ? GPUPipeline::BindRenderTargetsAsImages : GPUPipeline::NoRenderPassFlags);
  }
  else
  {
    g_gpu_device->SetRenderTarget(m_vram_texture.get(), m_vram_depth_texture.get(),
                                  m_allow_shader_blend ? GPUPipeline::ColorFeedbackLoop :
                                                         GPUPipeline::NoRenderPassFlags);
  }
}

void GPU_HW::DeactivateROV()
{
  if (!m_rov_active)
    return;

  GL_INS("Deactivating ROV.");
  m_rov_active = false;
  SetVRAMRenderTarget();
}

void GPU_HW::DestroyBuffers()
{
  m_presenter.ClearDisplayTexture();

  DebugAssert((m_batch_vertex_ptr != nullptr) == (m_batch_index_ptr != nullptr));
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(0, 0);

  m_vram_upload_buffer.reset();
  m_vram_readback_download_texture.reset();
  g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_extract_depth_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_extract_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_read_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_depth_copy_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_depth_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_readback_texture));
}

bool GPU_HW::CompileCommonShaders(Error* error)
{
  const GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), m_supports_dual_source_blend,
                                   m_supports_framebuffer_fetch);

  // use a depth of 1, that way writes will reset the depth
  m_fullscreen_quad_vertex_shader = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                               shadergen.GenerateScreenQuadVertexShader(1.0f), error);
  if (!m_fullscreen_quad_vertex_shader)
    return false;

  GL_OBJECT_NAME(m_fullscreen_quad_vertex_shader, "Fullscreen Quad Vertex Shader");

  m_screen_quad_vertex_shader = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                           shadergen.GenerateScreenVertexShader(), error);
  if (!m_screen_quad_vertex_shader)
    return false;

  GL_OBJECT_NAME(m_screen_quad_vertex_shader, "Screen Quad Vertex Shader");

  return true;
}

bool GPU_HW::CompilePipelines(Error* error)
{
  const GPUDevice::Features features = g_gpu_device->GetFeatures();
  const bool upscaled = (m_resolution_scale > 1);
  const bool msaa = (m_multisamples > 1);
  const bool per_sample_shading = (msaa && g_gpu_settings.gpu_per_sample_shading && features.per_sample_shading);
  const bool force_round_texcoords =
    (upscaled && m_texture_filtering == GPUTextureFilter::Nearest && g_gpu_settings.gpu_force_round_texcoords);
  const bool true_color = g_gpu_settings.IsUsingTrueColor();
  const bool scaled_dithering = (!m_true_color && upscaled && g_gpu_settings.IsUsingScaledDithering());
  const bool scaled_interlacing = (upscaled && g_gpu_settings.gpu_scaled_interlacing);
  const bool disable_color_perspective = (features.noperspective_interpolation && ShouldDisableColorPerspective());
  const bool needs_page_texture = m_use_texture_cache;
  const bool force_progressive_scan =
    (g_gpu_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);

  // Determine when to use shader blending.
  // FBFetch is free, we need it for filtering without DSB, or when accurate blending is forced.
  // But, don't bother with accurate blending if true colour is on. The result will be the same.
  // Prefer ROV over barriers/feedback loops without FBFetch, it'll be faster.
  // Abuse the depth buffer for the mask bit when it's free (FBFetch), or PGXP depth buffering is enabled.
  m_allow_shader_blend = features.framebuffer_fetch ||
                         ((features.feedback_loops || features.raster_order_views) &&
                          (m_pgxp_depth_buffer || g_gpu_settings.IsUsingShaderBlending() ||
                           (!m_supports_dual_source_blend && (IsBlendedTextureFiltering(m_texture_filtering) ||
                                                              IsBlendedTextureFiltering(m_sprite_texture_filtering)))));
  m_prefer_shader_blend = (m_allow_shader_blend && g_gpu_settings.IsUsingShaderBlending());
  m_use_rov_for_shader_blend = (m_allow_shader_blend && !features.framebuffer_fetch && features.raster_order_views &&
                                (m_prefer_shader_blend || !features.feedback_loops));
  m_write_mask_as_depth = (!m_pgxp_depth_buffer && !features.framebuffer_fetch && !m_prefer_shader_blend);

  // ROV doesn't support MSAA in DirectX.
  Assert(!m_use_rov_for_shader_blend || !IsUsingMultisampling());

  const bool needs_depth_buffer = (m_pgxp_depth_buffer || m_write_mask_as_depth);
  const bool needs_rov_depth = (m_pgxp_depth_buffer && m_use_rov_for_shader_blend);
  const bool needs_real_depth_buffer = (needs_depth_buffer && !needs_rov_depth);
  const bool needs_feedback_loop = (m_allow_shader_blend && features.feedback_loops && !m_use_rov_for_shader_blend);
  const GPUTexture::Format depth_buffer_format =
    needs_depth_buffer ? GetDepthBufferFormat() : GPUTexture::Format::Unknown;

  // Logging in case something goes wrong.
  INFO_LOG("Shader blending allowed: {}", m_allow_shader_blend ? "YES" : "NO");
  INFO_LOG("Shader blending preferred: {}", m_prefer_shader_blend ? "YES" : "NO");
  INFO_LOG("Use ROV for shader blending: {}", m_use_rov_for_shader_blend ? "YES" : "NO");
  INFO_LOG("Write mask as depth: {}", m_write_mask_as_depth ? "YES" : "NO");
  INFO_LOG("Depth buffer is {}needed in {}.", needs_depth_buffer ? "" : "NOT ",
           GPUTexture::GetFormatName(GetDepthBufferFormat()));
  INFO_LOG("Using ROV depth: {}", needs_rov_depth ? "YES" : "NO");
  INFO_LOG("Using real depth buffer: {}", needs_real_depth_buffer ? "YES" : "NO");
  INFO_LOG("Using feedback loops: {}", needs_feedback_loop ? "YES" : "NO");

  // Start generating shaders.
  const GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), m_supports_dual_source_blend,
                                   m_supports_framebuffer_fetch);

  const u32 max_active_texture_modes =
    (m_allow_sprite_mode ? NUM_TEXTURE_MODES :
                           (NUM_TEXTURE_MODES - (NUM_TEXTURE_MODES - static_cast<u32>(BatchTextureMode::SpriteStart))));
  const u32 num_active_texture_modes =
    (max_active_texture_modes - (BoolToUInt32(!needs_page_texture) * (BoolToUInt32(m_allow_sprite_mode) + 1)));
  const u32 total_vertex_shaders =
    ((m_allow_sprite_mode ? 7 : 4) - (BoolToUInt32(!needs_page_texture) * (BoolToUInt32(m_allow_sprite_mode) + 1)));
  const u32 total_fragment_shaders = ((1 + BoolToUInt32(needs_rov_depth)) * 5 * 5 * num_active_texture_modes * 2 *
                                      (1 + BoolToUInt32(!true_color)) * (1 + BoolToUInt32(!force_progressive_scan)));
  const u32 total_items =
    total_vertex_shaders + total_fragment_shaders +
    ((m_pgxp_depth_buffer ? 2 : 1) * 5 * 5 * num_active_texture_modes * 2 * (1 + BoolToUInt32(!true_color)) *
     (1 + BoolToUInt32(!force_progressive_scan))) +              // batch pipelines
    ((m_wireframe_mode != GPUWireframeMode::Disabled) ? 1 : 0) + // wireframe
    (2 * 2) +                                                    // vram fill
    (1 + BoolToUInt32(m_write_mask_as_depth)) +                  // vram copy
    (1 + BoolToUInt32(m_write_mask_as_depth)) +                  // vram write
    1 +                                                          // vram write replacement
    (m_write_mask_as_depth ? 1 : 0) +                            // mask -> depth
    1;                                                           // resolution dependent shaders

  INFO_LOG("Compiling {} vertex shaders, {} fragment shaders, and {} pipelines.", total_vertex_shaders,
           total_fragment_shaders, total_items);

  // destroy old pipelines, if any
  m_wireframe_pipeline.reset();
  m_batch_pipelines.enumerate([](std::unique_ptr<GPUPipeline>& p) { p.reset(); });
  m_vram_fill_pipelines.enumerate([](std::unique_ptr<GPUPipeline>& p) { p.reset(); });
  for (std::unique_ptr<GPUPipeline>& p : m_vram_write_pipelines)
    p.reset();
  for (std::unique_ptr<GPUPipeline>& p : m_vram_copy_pipelines)
    p.reset();
  m_vram_update_depth_pipeline.reset();
  m_vram_write_replacement_pipeline.reset();
  m_clear_depth_pipeline.reset();
  m_copy_depth_pipeline.reset();

  ShaderCompileProgressTracker progress(total_items);

  // vertex shaders - [textured/palette/sprite]
  // fragment shaders - [depth_test][render_mode][transparency_mode][texture_mode][check_mask][dithering][interlacing]
  static constexpr auto destroy_shader = [](std::unique_ptr<GPUShader>& s) { s.reset(); };
  DimensionalArray<std::unique_ptr<GPUShader>, 2, 3, 2> batch_vertex_shaders{};
  DimensionalArray<std::unique_ptr<GPUShader>, 2, 2, 2, NUM_TEXTURE_MODES, 5, 5, 2> batch_fragment_shaders{};
  ScopedGuard batch_shader_guard([&batch_vertex_shaders, &batch_fragment_shaders]() {
    batch_vertex_shaders.enumerate(destroy_shader);
    batch_fragment_shaders.enumerate(destroy_shader);
  });

  for (u8 textured = 0; textured < 2; textured++)
  {
    for (u8 palette = 0; palette < 3; palette++)
    {
      if (palette && !textured)
        continue;
      if (palette == 2 && !needs_page_texture)
        continue;

      for (u8 sprite = 0; sprite < 2; sprite++)
      {
        if (sprite && (!textured || !m_allow_sprite_mode))
          continue;

        const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
        const std::string vs = shadergen.GenerateBatchVertexShader(
          upscaled, msaa, per_sample_shading, textured != 0, palette == 1, palette == 2, uv_limits,
          !sprite && force_round_texcoords, m_pgxp_depth_buffer, disable_color_perspective);
        if (!(batch_vertex_shaders[textured][palette][sprite] =
                g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(), vs, error)))
        {
          return false;
        }

        if (!progress.Increment(1, error)) [[unlikely]]
          return false;
      }
    }
  }

  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    if (depth_test && !needs_rov_depth)
    {
      // Don't need to do depth testing in the shader.
      continue;
    }

    for (u8 render_mode = 0; render_mode < 5; render_mode++)
    {
      for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
      {
        if (
          // Can't generate shader blending.
          ((render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && !m_allow_shader_blend) ||
           (render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend) &&
            transparency_mode != static_cast<u8>(GPUTransparencyMode::Disabled))) ||
          // Don't need multipass shaders if we're preferring shader blend or have (free) FBFetch.
          ((m_supports_framebuffer_fetch || m_prefer_shader_blend) &&
           (render_mode == static_cast<u8>(BatchRenderMode::OnlyOpaque) ||
            render_mode == static_cast<u8>(BatchRenderMode::OnlyTransparent))) ||
          // If using ROV depth, we only draw with shader blending.
          (needs_rov_depth && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend)))
        {
          if (!progress.Increment(num_active_texture_modes * 2 * (1 + BoolToUInt32(!true_color)) *
                                    (1 + BoolToUInt32(!force_progressive_scan)),
                                  error)) [[unlikely]]
          {
            return false;
          }

          continue;
        }

        for (u8 texture_mode = 0; texture_mode < max_active_texture_modes; texture_mode++)
        {
          if (!needs_page_texture && (texture_mode == static_cast<u8>(BatchTextureMode::PageTexture) ||
                                      texture_mode == static_cast<u8>(BatchTextureMode::SpritePageTexture)))
          {
            continue;
          }

          for (u8 check_mask = 0; check_mask < 2; check_mask++)
          {
            if (check_mask && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend))
            {
              // mask bit testing is only valid with shader blending.
              if (!progress.Increment((1 + BoolToUInt32(!true_color)) * (1 + BoolToUInt32(!force_progressive_scan)),
                                      error)) [[unlikely]]
              {
                return false;
              }

              continue;
            }

            for (u8 dithering = 0; dithering < 2; dithering++)
            {
              // Never going to draw with dithering on in true color.
              if (dithering && true_color)
                continue;

              for (u8 interlacing = 0; interlacing < 2; interlacing++)
              {
                // Never going to draw with line skipping in force progressive.
                if (interlacing && force_progressive_scan)
                  continue;

                const bool sprite = (static_cast<BatchTextureMode>(texture_mode) >= BatchTextureMode::SpriteStart);
                const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
                const BatchTextureMode shader_texmode = static_cast<BatchTextureMode>(
                  texture_mode - (sprite ? static_cast<u8>(BatchTextureMode::SpriteStart) : 0));
                const GPUTextureFilter texture_filter = sprite ? m_sprite_texture_filtering : m_texture_filtering;
                const bool texture_filter_is_blended =
                  (shader_texmode != BatchTextureMode::Disabled && IsBlendedTextureFiltering(texture_filter));
                const bool use_rov =
                  (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && m_use_rov_for_shader_blend);
                const bool rov_depth_test = (use_rov && depth_test != 0);
                const bool rov_depth_write = (rov_depth_test && static_cast<GPUTransparencyMode>(transparency_mode) ==
                                                                  GPUTransparencyMode::Disabled);
                const std::string fs = shadergen.GenerateBatchFragmentShader(
                  static_cast<BatchRenderMode>(render_mode), static_cast<GPUTransparencyMode>(transparency_mode),
                  shader_texmode, texture_filter, texture_filter_is_blended, upscaled, msaa, per_sample_shading,
                  uv_limits, !sprite && force_round_texcoords, true_color, ConvertToBoolUnchecked(dithering),
                  scaled_dithering, disable_color_perspective, ConvertToBoolUnchecked(interlacing), scaled_interlacing,
                  ConvertToBoolUnchecked(check_mask), m_write_mask_as_depth, use_rov, needs_rov_depth, rov_depth_test,
                  rov_depth_write);

                if (!(batch_fragment_shaders[depth_test][render_mode][transparency_mode][texture_mode][check_mask]
                                            [dithering][interlacing] = g_gpu_device->CreateShader(
                                              GPUShaderStage::Fragment, shadergen.GetLanguage(), fs, error)))
                {
                  return false;
                }

                if (!progress.Increment(1, error)) [[unlikely]]
                  return false;
              }
            }
          }
        }
      }
    }
  }

  static constexpr GPUPipeline::VertexAttribute vertex_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 4, OFFSETOF(BatchVertex, x)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::Color, 0,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(BatchVertex, color)),
    GPUPipeline::VertexAttribute::Make(2, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::UInt32, 1, OFFSETOF(BatchVertex, u)),
    GPUPipeline::VertexAttribute::Make(3, GPUPipeline::VertexAttribute::Semantic::TexCoord, 1,
                                       GPUPipeline::VertexAttribute::Type::UInt32, 1, OFFSETOF(BatchVertex, texpage)),
    GPUPipeline::VertexAttribute::Make(4, GPUPipeline::VertexAttribute::Semantic::TexCoord, 2,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(BatchVertex, uv_limits)),
  };
  static constexpr u32 NUM_BATCH_VERTEX_ATTRIBUTES = 2;
  static constexpr u32 NUM_BATCH_TEXTURED_VERTEX_ATTRIBUTES = 4;
  static constexpr u32 NUM_BATCH_TEXTURED_LIMITS_VERTEX_ATTRIBUTES = 5;

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndUBO;
  plconfig.input_layout.vertex_stride = sizeof(BatchVertex);
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.geometry_shader = nullptr;
  plconfig.samples = m_multisamples;
  plconfig.per_sample_shading = per_sample_shading;
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();

  // [depth_test][transparency_mode][render_mode][texture_mode][dithering][interlacing][check_mask]
  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    if (depth_test && !m_pgxp_depth_buffer)
    {
      // Not used.
      continue;
    }

    for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
    {
      for (u8 render_mode = 0; render_mode < 5; render_mode++)
      {
        if (
          // Can't generate shader blending.
          (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && !m_allow_shader_blend) ||
          // Don't need multipass shaders.
          ((m_supports_framebuffer_fetch || m_prefer_shader_blend) &&
           (render_mode == static_cast<u8>(BatchRenderMode::OnlyOpaque) ||
            render_mode == static_cast<u8>(BatchRenderMode::OnlyTransparent))) ||
          // If using ROV depth, we only draw with shader blending.
          (needs_rov_depth && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend)))
        {
          if (!progress.Increment(num_active_texture_modes * 2 * (1 + BoolToUInt32(!true_color)) *
                                    (1 + BoolToUInt32(!force_progressive_scan)),
                                  error)) [[unlikely]]
          {
            return false;
          }

          continue;
        }

        for (u8 texture_mode = 0; texture_mode < max_active_texture_modes; texture_mode++)
        {
          if (!needs_page_texture && (texture_mode == static_cast<u8>(BatchTextureMode::PageTexture) ||
                                      texture_mode == static_cast<u8>(BatchTextureMode::SpritePageTexture)))
          {
            continue;
          }

          for (u8 dithering = 0; dithering < 2; dithering++)
          {
            // Never going to draw with dithering on in true color.
            if (dithering && true_color)
              continue;

            for (u8 interlacing = 0; interlacing < 2; interlacing++)
            {
              // Never going to draw with line skipping in force progressive.
              if (interlacing && force_progressive_scan)
                continue;

              for (u8 check_mask = 0; check_mask < 2; check_mask++)
              {
                const bool textured = (static_cast<BatchTextureMode>(texture_mode) != BatchTextureMode::Disabled);
                const bool palette =
                  (static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::Palette4Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::Palette8Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::SpritePalette4Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::SpritePalette8Bit);
                const bool page_texture =
                  (static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::PageTexture ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::SpritePageTexture);
                const bool sprite = (static_cast<BatchTextureMode>(texture_mode) >= BatchTextureMode::SpriteStart);
                const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
                const bool use_shader_blending = (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend));
                const bool use_rov = (use_shader_blending && m_use_rov_for_shader_blend);
                plconfig.input_layout.vertex_attributes =
                  textured ?
                    (uv_limits ? std::span<const GPUPipeline::VertexAttribute>(
                                   vertex_attributes, NUM_BATCH_TEXTURED_LIMITS_VERTEX_ATTRIBUTES) :
                                 std::span<const GPUPipeline::VertexAttribute>(vertex_attributes,
                                                                               NUM_BATCH_TEXTURED_VERTEX_ATTRIBUTES)) :
                    std::span<const GPUPipeline::VertexAttribute>(vertex_attributes, NUM_BATCH_VERTEX_ATTRIBUTES);

                plconfig.vertex_shader =
                  batch_vertex_shaders[BoolToUInt8(textured)][page_texture ? 2 : BoolToUInt8(palette)]
                                      [BoolToUInt8(sprite)]
                                        .get();
                plconfig.fragment_shader =
                  batch_fragment_shaders[BoolToUInt8(depth_test && needs_rov_depth)][render_mode]
                                        [use_shader_blending ? transparency_mode :
                                                               static_cast<u8>(GPUTransparencyMode::Disabled)]
                                        [texture_mode][use_shader_blending ? check_mask : 0][dithering][interlacing]
                                          .get();
                Assert(plconfig.vertex_shader && plconfig.fragment_shader);

                if (needs_real_depth_buffer)
                {
                  plconfig.depth.depth_test =
                    m_pgxp_depth_buffer ?
                      (depth_test ? GPUPipeline::DepthFunc::LessEqual : GPUPipeline::DepthFunc::Always) :
                      (check_mask ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always);

                  // Don't write for transparent, but still test.
                  plconfig.depth.depth_write =
                    !m_pgxp_depth_buffer ||
                    (depth_test && transparency_mode == static_cast<u8>(GPUTransparencyMode::Disabled));
                }

                plconfig.SetTargetFormats(use_rov ? GPUTexture::Format::Unknown : VRAM_RT_FORMAT,
                                          needs_rov_depth ? GPUTexture::Format::Unknown : depth_buffer_format);
                plconfig.color_formats[1] = needs_rov_depth ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;
                plconfig.render_pass_flags =
                  use_rov ? GPUPipeline::BindRenderTargetsAsImages :
                            (needs_feedback_loop ? GPUPipeline::ColorFeedbackLoop : GPUPipeline::NoRenderPassFlags);

                plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();

                if (use_rov)
                {
                  plconfig.blend.write_mask = 0;
                }
                else if (!use_shader_blending &&
                         ((static_cast<GPUTransparencyMode>(transparency_mode) != GPUTransparencyMode::Disabled &&
                           (static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                            static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque)) ||
                          (textured &&
                           IsBlendedTextureFiltering(sprite ? m_sprite_texture_filtering : m_texture_filtering))))
                {
                  plconfig.blend.enable = true;
                  plconfig.blend.src_alpha_blend = GPUPipeline::BlendFunc::One;
                  plconfig.blend.dst_alpha_blend = GPUPipeline::BlendFunc::Zero;
                  plconfig.blend.alpha_blend_op = GPUPipeline::BlendOp::Add;

                  if (m_supports_dual_source_blend)
                  {
                    plconfig.blend.src_blend = GPUPipeline::BlendFunc::One;
                    plconfig.blend.dst_blend = GPUPipeline::BlendFunc::SrcAlpha1;
                    plconfig.blend.blend_op =
                      (static_cast<GPUTransparencyMode>(transparency_mode) ==
                         GPUTransparencyMode::BackgroundMinusForeground &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                        GPUPipeline::BlendOp::ReverseSubtract :
                        GPUPipeline::BlendOp::Add;
                  }
                  else
                  {
                    // TODO: This isn't entirely accurate, 127.5 versus 128.
                    // But if we use fbfetch on Mali, it doesn't matter.
                    plconfig.blend.src_blend = GPUPipeline::BlendFunc::One;
                    plconfig.blend.dst_blend = GPUPipeline::BlendFunc::One;
                    if (static_cast<GPUTransparencyMode>(transparency_mode) ==
                        GPUTransparencyMode::HalfBackgroundPlusHalfForeground)
                    {
                      plconfig.blend.dst_blend = GPUPipeline::BlendFunc::ConstantColor;
                      plconfig.blend.dst_alpha_blend = GPUPipeline::BlendFunc::ConstantColor;
                      plconfig.blend.constant = 0x00808080u;
                    }

                    plconfig.blend.blend_op =
                      (static_cast<GPUTransparencyMode>(transparency_mode) ==
                         GPUTransparencyMode::BackgroundMinusForeground &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                        GPUPipeline::BlendOp::ReverseSubtract :
                        GPUPipeline::BlendOp::Add;
                  }
                }

                if (!(m_batch_pipelines[depth_test][transparency_mode][render_mode][texture_mode][dithering]
                                       [interlacing][check_mask] = g_gpu_device->CreatePipeline(plconfig, error)))
                {
                  return false;
                }

                if (!progress.Increment(1, error)) [[unlikely]]
                  return false;
              }
            }
          }
        }
      }
    }
  }

  plconfig.SetTargetFormats(VRAM_RT_FORMAT, needs_rov_depth ? GPUTexture::Format::Unknown : depth_buffer_format);
  plconfig.render_pass_flags = needs_feedback_loop ? GPUPipeline::ColorFeedbackLoop : GPUPipeline::NoRenderPassFlags;

  if (m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    std::unique_ptr<GPUShader> gs = g_gpu_device->CreateShader(GPUShaderStage::Geometry, shadergen.GetLanguage(),
                                                               shadergen.GenerateWireframeGeometryShader(), error);
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateWireframeFragmentShader(), error);
    if (!gs || !fs)
      return false;

    GL_OBJECT_NAME(gs, "Batch Wireframe Geometry Shader");
    GL_OBJECT_NAME(fs, "Batch Wireframe Fragment Shader");

    plconfig.input_layout.vertex_attributes =
      std::span<const GPUPipeline::VertexAttribute>(vertex_attributes, NUM_BATCH_VERTEX_ATTRIBUTES);
    plconfig.blend = (m_wireframe_mode == GPUWireframeMode::OverlayWireframe) ?
                       GPUPipeline::BlendState::GetAlphaBlendingState() :
                       GPUPipeline::BlendState::GetNoBlendingState();
    plconfig.blend.write_mask = 0x7;
    plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
    plconfig.vertex_shader = batch_vertex_shaders[0][0][0].get();
    plconfig.geometry_shader = gs.get();
    plconfig.fragment_shader = fs.get();

    if (!(m_wireframe_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_wireframe_pipeline, "Batch Wireframe Pipeline");

    plconfig.vertex_shader = nullptr;
    plconfig.geometry_shader = nullptr;
    plconfig.fragment_shader = nullptr;

    if (!progress.Increment(1, error)) [[unlikely]]
      return false;
  }

  batch_shader_guard.Run();

  // common state
  SetScreenQuadInputLayout(plconfig);
  plconfig.vertex_shader = m_screen_quad_vertex_shader.get();
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.per_sample_shading = false;
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.color_formats[1] = needs_rov_depth ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;

  // VRAM fill
  for (u8 wrapped = 0; wrapped < 2; wrapped++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(),
        shadergen.GenerateVRAMFillFragmentShader(ConvertToBoolUnchecked(wrapped), ConvertToBoolUnchecked(interlaced),
                                                 m_write_mask_as_depth, needs_rov_depth),
        error);
      if (!fs)
        return false;

      plconfig.fragment_shader = fs.get();
      plconfig.depth = needs_real_depth_buffer ? GPUPipeline::DepthState::GetAlwaysWriteState() :
                                                 GPUPipeline::DepthState::GetNoTestsState();

      if (!(m_vram_fill_pipelines[wrapped][interlaced] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      if (!progress.Increment(1, error)) [[unlikely]]
        return false;
    }
  }

  // VRAM copy
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(),
      shadergen.GenerateVRAMCopyFragmentShader(m_write_mask_as_depth, needs_rov_depth), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      if (depth_test && !m_write_mask_as_depth)
        continue;

      plconfig.depth.depth_write = needs_real_depth_buffer;
      plconfig.depth.depth_test =
        (depth_test != 0) ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always;

      if (!(m_vram_copy_pipelines[depth_test] = g_gpu_device->CreatePipeline(plconfig), error))
        return false;

      GL_OBJECT_NAME_FMT(m_vram_copy_pipelines[depth_test], "VRAM Write Pipeline, depth={}", depth_test);

      if (!progress.Increment(1, error)) [[unlikely]]
        return false;
    }
  }

  // VRAM write
  {
    const bool use_buffer = features.texture_buffers;
    const bool use_ssbo = features.texture_buffers_emulated_with_ssbo;
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(),
      shadergen.GenerateVRAMWriteFragmentShader(use_buffer, use_ssbo, m_write_mask_as_depth, needs_rov_depth), error);
    if (!fs)
      return false;

    plconfig.layout = use_buffer ? GPUPipeline::Layout::SingleTextureBufferAndPushConstants :
                                   GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.fragment_shader = fs.get();
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      if (depth_test && !m_write_mask_as_depth)
        continue;

      plconfig.depth.depth_write = needs_real_depth_buffer;
      plconfig.depth.depth_test =
        (depth_test != 0) ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always;

      if (!(m_vram_write_pipelines[depth_test] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      GL_OBJECT_NAME_FMT(m_vram_write_pipelines[depth_test], "VRAM Write Pipeline, depth={}", depth_test);

      if (!progress.Increment(1, error)) [[unlikely]]
        return false;
    }
  }

  // VRAM write replacement
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateVRAMReplacementBlitFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
    if (!(m_vram_write_replacement_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    if (!progress.Increment(1, error)) [[unlikely]]
      return false;
  }

  plconfig.vertex_shader = m_fullscreen_quad_vertex_shader.get();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.input_layout.vertex_attributes = {};
  plconfig.input_layout.vertex_stride = 0;

  // VRAM update depth
  if (m_write_mask_as_depth)
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateVRAMUpdateDepthFragmentShader(msaa), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(GPUTexture::Format::Unknown, depth_buffer_format);
    plconfig.depth = GPUPipeline::DepthState::GetAlwaysWriteState();
    plconfig.blend.write_mask = 0;

    if (!(m_vram_update_depth_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_vram_update_depth_pipeline, "VRAM Update Depth Pipeline");

    if (!progress.Increment(1, error)) [[unlikely]]
      return false;
  }

  if (m_pgxp_depth_buffer)
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateVRAMCopyDepthFragmentShader(msaa), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
    plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
    plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
    plconfig.samples = m_multisamples;
    plconfig.per_sample_shading = true;
    plconfig.SetTargetFormats(VRAM_DS_COLOR_FORMAT);
    if (!(m_copy_depth_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateVRAMClearDepthFragmentShader(m_use_rov_for_shader_blend), error);
    if (!fs)
      return false;

    SetScreenQuadInputLayout(plconfig);
    plconfig.vertex_shader = m_screen_quad_vertex_shader.get();
    plconfig.fragment_shader = fs.get();
    plconfig.per_sample_shading = false;
    if (!m_use_rov_for_shader_blend)
    {
      plconfig.SetTargetFormats(VRAM_RT_FORMAT, depth_buffer_format);
      plconfig.render_pass_flags =
        needs_feedback_loop ? GPUPipeline::ColorFeedbackLoop : GPUPipeline::NoRenderPassFlags;
      plconfig.blend.write_mask = 0;
      plconfig.depth = GPUPipeline::DepthState::GetAlwaysWriteState();
    }
    else
    {
      plconfig.SetTargetFormats(depth_buffer_format);
    }

    if (!(m_clear_depth_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
  }

  if (!CompileResolutionDependentPipelines(error) || !CompileDownsamplePipelines(error))
    return false;

  if (!progress.Increment(1, error)) [[unlikely]]
    return false;

#undef UPDATE_PROGRESS

  INFO_LOG("Pipeline creation took {:.2f} ms.", progress.GetElapsedMilliseconds());
  return true;
}

bool GPU_HW::CompileResolutionDependentPipelines(Error* error)
{
  Timer timer;

  m_vram_readback_pipeline.reset();
  for (std::unique_ptr<GPUPipeline>& p : m_vram_extract_pipeline)
    p.reset();

  const GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), m_supports_dual_source_blend,
                                   m_supports_framebuffer_fetch);

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.input_layout.vertex_attributes = {};
  plconfig.input_layout.vertex_stride = 0;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.geometry_shader = nullptr;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.vertex_shader = m_fullscreen_quad_vertex_shader.get();
  plconfig.SetTargetFormats(VRAM_RT_FORMAT);

  // VRAM read
  {
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateVRAMReadFragmentShader(m_resolution_scale, m_multisamples), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();

    if (!(m_vram_readback_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_vram_readback_pipeline, "VRAM Read Pipeline");
  }

  // Display
  {
    for (u8 shader = 0; shader < 3; shader++)
    {
      // 24-bit doesn't give you a depth buffer.
      const bool color_24bit = (shader == 1);
      const bool depth_extract = (shader == 2);
      if (depth_extract && !m_pgxp_depth_buffer)
        continue;

      std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(),
        shadergen.GenerateVRAMExtractFragmentShader(m_resolution_scale, m_multisamples, color_24bit, depth_extract),
        error);
      if (!fs)
        return false;

      plconfig.fragment_shader = fs.get();

      plconfig.layout = depth_extract ? GPUPipeline::Layout::MultiTextureAndPushConstants :
                                        GPUPipeline::Layout::SingleTextureAndPushConstants;
      plconfig.color_formats[1] = depth_extract ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;

      if (!(m_vram_extract_pipeline[shader] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      GL_OBJECT_NAME_FMT(m_vram_readback_pipeline, "VRAM Extract Pipeline 24bit={} Depth={}", color_24bit,
                         depth_extract);
    }
  }

  INFO_LOG("Compiling resolution dependent pipelines took {:.2f} ms.", timer.GetTimeMilliseconds());
  return true;
}

bool GPU_HW::CompileDownsamplePipelines(Error* error)
{
  m_downsample_pass_pipeline.reset();
  m_downsample_blur_pipeline.reset();
  m_downsample_composite_pipeline.reset();
  m_downsample_lod_sampler.reset();
  m_downsample_composite_sampler.reset();

  if (m_downsample_mode == GPUDownsampleMode::Disabled)
    return true;

  const GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), m_supports_dual_source_blend,
                                   m_supports_framebuffer_fetch);

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.input_layout.vertex_attributes = {};
  plconfig.input_layout.vertex_stride = 0;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.geometry_shader = nullptr;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.vertex_shader = m_fullscreen_quad_vertex_shader.get();
  plconfig.SetTargetFormats(VRAM_RT_FORMAT);

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(
      GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateAdaptiveDownsampleVertexShader(), error);
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateAdaptiveDownsampleMipFragmentShader(), error);
    if (!vs || !fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Vertex Shader");
    GL_OBJECT_NAME(fs, "Downsample Fragment Shader");
    plconfig.vertex_shader = vs.get();
    plconfig.fragment_shader = fs.get();
    if (!(m_downsample_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_pass_pipeline, "Downsample First Pass Pipeline");

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateAdaptiveDownsampleBlurFragmentShader(), error);
    if (!fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Blur Fragment Shader");
    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(GPUTexture::Format::R8);
    if (!(m_downsample_blur_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_blur_pipeline, "Downsample Blur Pass Pipeline");

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateAdaptiveDownsampleCompositeFragmentShader(), error);
    if (!fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Composite Fragment Shader");
    plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(VRAM_RT_FORMAT);
    if (!(m_downsample_composite_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_composite_pipeline, "Downsample Blur Pass Pipeline");

    GPUSampler::Config config = GPUSampler::GetLinearConfig();
    config.min_lod = 0;
    config.max_lod = GPUSampler::Config::LOD_MAX;
    if (!(m_downsample_lod_sampler = g_gpu_device->CreateSampler(config)))
    {
      Error::SetStringView(error, "Failed to create downsample LOD sampler.");
      return false;
    }
    GL_OBJECT_NAME(m_downsample_lod_sampler, "Downsample LOD Sampler");
    config.mip_filter = GPUSampler::Filter::Linear;
    if (!(m_downsample_composite_sampler = g_gpu_device->CreateSampler(config)))
    {
      Error::SetStringView(error, "Failed to create downsample composite sampler.");
      return false;
    }
    GL_OBJECT_NAME(m_downsample_composite_sampler, "Downsample Trilinear Sampler");
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateBoxSampleDownsampleFragmentShader(
                                   m_resolution_scale / GetBoxDownsampleScale(m_resolution_scale)),
                                 error);
    if (!fs)
      return false;

    GL_OBJECT_NAME(fs, "Box Downsample Fragment Shader");
    plconfig.fragment_shader = fs.get();

    if (!(m_downsample_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_downsample_pass_pipeline, "Box Downsample Pipeline");
  }

  return true;
}

GPU_HW::BatchRenderMode GPU_HW::BatchConfig::GetRenderMode() const
{
  return transparency_mode == GPUTransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                              BatchRenderMode::TransparentAndOpaque;
}

void GPU_HW::UpdateVRAMReadTexture(bool drawn, bool written)
{
  GL_SCOPE("UpdateVRAMReadTexture()");

  const auto update = [this](GSVector4i& rect, u8 dbit) {
    if (m_texpage_dirty & dbit)
    {
      m_texpage_dirty &= ~dbit;
      if (!m_texpage_dirty)
        GL_INS_FMT("{} texpage is no longer dirty", (dbit & TEXPAGE_DIRTY_DRAWN_RECT) ? "DRAW" : "WRITE");
    }

    const GSVector4i scaled_rect = rect.mul32l(GSVector4i(m_resolution_scale));
    if (m_vram_texture->IsMultisampled())
    {
      if (g_gpu_device->GetFeatures().partial_msaa_resolve)
      {
        g_gpu_device->ResolveTextureRegion(m_vram_read_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                           m_vram_texture.get(), scaled_rect.left, scaled_rect.top, scaled_rect.width(),
                                           scaled_rect.height());
      }
      else
      {
        g_gpu_device->ResolveTextureRegion(m_vram_read_texture.get(), 0, 0, 0, 0, m_vram_texture.get(), 0, 0,
                                           m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
      }
    }
    else
    {
      g_gpu_device->CopyTextureRegion(m_vram_read_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                      m_vram_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                      scaled_rect.width(), scaled_rect.height());
    }

    // m_counters.num_read_texture_updates++;
    rect = INVALID_RECT;
  };

  if (drawn)
  {
    DebugAssert(!m_vram_dirty_draw_rect.eq(INVALID_RECT));
    GL_INS_FMT("Updating draw rect {}", m_vram_dirty_draw_rect);

    u8 dbits = TEXPAGE_DIRTY_DRAWN_RECT;
    if (written && m_vram_dirty_draw_rect.rintersects(m_vram_dirty_write_rect))
    {
      DebugAssert(!m_vram_dirty_write_rect.eq(INVALID_RECT));
      GL_INS_FMT("Including write rect {}", m_vram_dirty_write_rect);
      m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(m_vram_dirty_write_rect);
      m_vram_dirty_write_rect = INVALID_RECT;
      dbits = TEXPAGE_DIRTY_DRAWN_RECT | TEXPAGE_DIRTY_WRITTEN_RECT;
      written = false;
    }

    update(m_vram_dirty_draw_rect, dbits);
  }
  if (written)
  {
    GL_INS_FMT("Updating write rect {}", m_vram_dirty_write_rect);
    update(m_vram_dirty_write_rect, TEXPAGE_DIRTY_WRITTEN_RECT);
  }
}

void GPU_HW::UpdateDepthBufferFromMaskBit()
{
  GL_SCOPE_FMT("UpdateDepthBufferFromMaskBit()");
  DebugAssert(!m_pgxp_depth_buffer && m_vram_depth_texture && m_write_mask_as_depth);

  // Viewport should already be set full, only need to fudge the scissor.
  g_gpu_device->SetScissor(m_vram_texture->GetRect());
  g_gpu_device->InvalidateRenderTarget(m_vram_depth_texture.get());
  g_gpu_device->SetRenderTargets(nullptr, 0, m_vram_depth_texture.get());
  g_gpu_device->SetPipeline(m_vram_update_depth_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
  g_gpu_device->Draw(3, 0);

  // Restore.
  g_gpu_device->SetTextureSampler(0, m_vram_read_texture.get(), g_gpu_device->GetNearestSampler());
  SetVRAMRenderTarget();
  SetScissor();
}

void GPU_HW::CopyAndClearDepthBuffer(bool only_drawing_area)
{
  if (!m_depth_was_copied)
  {
    // Take a copy of the current depth buffer so it can be used when the previous frame/buffer gets scanned out.
    // Don't bother when we're not postprocessing, it'd just be a wasted copy.
    if (m_internal_postfx && m_internal_postfx->NeedsDepthBuffer())
    {
      // TODO: Shrink this to only the active area.
      GL_SCOPE("Copy Depth Buffer");

      m_vram_texture->MakeReadyForSampling();
      g_gpu_device->InvalidateRenderTarget(m_vram_depth_copy_texture.get());
      g_gpu_device->SetRenderTarget(m_vram_depth_copy_texture.get());
      g_gpu_device->SetViewportAndScissor(0, 0, m_vram_depth_texture->GetWidth(), m_vram_depth_texture->GetHeight());
      g_gpu_device->SetTextureSampler(0, m_vram_depth_texture.get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetPipeline(m_copy_depth_pipeline.get());

      const float uniforms[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));
      RestoreDeviceContext();
    }

    m_depth_was_copied = true;
  }

  ClearDepthBuffer(only_drawing_area);
}

void GPU_HW::ClearDepthBuffer(bool only_drawing_area)
{
  GL_SCOPE_FMT("GPU_HW::ClearDepthBuffer({})", only_drawing_area ? "Only Drawing Area" : "Full Buffer");
  DebugAssert(m_pgxp_depth_buffer);
  if (only_drawing_area)
  {
    g_gpu_device->SetPipeline(m_clear_depth_pipeline.get());

    const GSVector4i clear_bounds = m_clamped_drawing_area.mul32l(GSVector4i(m_resolution_scale));

    // need to re-bind for rov, because we can't turn colour writes off for only the first target
    if (!m_use_rov_for_shader_blend)
    {
      DrawScreenQuad(clear_bounds, m_vram_depth_texture->GetSizeVec(), GSVector4::zero(), nullptr, 0);
    }
    else
    {
      g_gpu_device->SetRenderTarget(m_vram_depth_texture.get());
      DrawScreenQuad(clear_bounds, m_vram_depth_texture->GetSizeVec(), GSVector4::zero(), nullptr, 0);
      SetVRAMRenderTarget();
    }
  }
  else
  {
    if (m_use_rov_for_shader_blend)
      g_gpu_device->ClearRenderTarget(m_vram_depth_texture.get(), 0xFF);
    else
      g_gpu_device->ClearDepth(m_vram_depth_texture.get(), 1.0f);
  }

  m_last_depth_z = 1.0f;
  s_counters.num_depth_buffer_clears++;
}

void GPU_HW::SetScissor()
{
  g_gpu_device->SetScissor(m_clamped_drawing_area.mul32l(GSVector4i(m_resolution_scale)));
}

void GPU_HW::MapGPUBuffer(u32 required_vertices, u32 required_indices)
{
  DebugAssert(!m_batch_vertex_ptr && !m_batch_index_ptr);

  void* vb_map;
  u32 vb_space;
  g_gpu_device->MapVertexBuffer(sizeof(BatchVertex), required_vertices, &vb_map, &vb_space, &m_batch_base_vertex);
  m_batch_vertex_ptr = static_cast<BatchVertex*>(vb_map);
  m_batch_vertex_space = Truncate16(std::min<u32>(vb_space, std::numeric_limits<u16>::max()));

  u32 ib_space;
  g_gpu_device->MapIndexBuffer(required_indices, &m_batch_index_ptr, &ib_space, &m_batch_base_index);
  m_batch_index_space = Truncate16(std::min<u32>(ib_space, std::numeric_limits<u16>::max()));
}

void GPU_HW::UnmapGPUBuffer(u32 used_vertices, u32 used_indices)
{
  DebugAssert(m_batch_vertex_ptr && m_batch_index_ptr);
  g_gpu_device->UnmapVertexBuffer(sizeof(BatchVertex), used_vertices);
  g_gpu_device->UnmapIndexBuffer(used_indices);
  m_batch_vertex_ptr = nullptr;
  m_batch_vertex_count = 0;
  m_batch_vertex_space = 0;
  m_batch_index_ptr = nullptr;
  m_batch_index_count = 0;
  m_batch_index_space = 0;
}

ALWAYS_INLINE_RELEASE void GPU_HW::DrawBatchVertices(BatchRenderMode render_mode, u32 num_indices, u32 base_index,
                                                     u32 base_vertex, const GPUTextureCache::Source* texture)
{
  // [depth_test][transparency_mode][render_mode][texture_mode][dithering][interlacing][check_mask]
  const u8 texture_mode = (static_cast<u8>(m_batch.texture_mode) +
                           ((m_batch.texture_mode < BatchTextureMode::Disabled && m_batch.sprite_mode) ?
                              static_cast<u8>(BatchTextureMode::SpriteStart) :
                              0));
  const u8 depth_test = BoolToUInt8(m_batch.use_depth_buffer);
  const u8 check_mask = BoolToUInt8(m_batch.check_mask_before_draw);
  g_gpu_device->SetPipeline(m_batch_pipelines[depth_test][static_cast<u8>(m_batch.transparency_mode)][static_cast<u8>(
    render_mode)][texture_mode][BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)][check_mask]
                              .get());

  if (m_use_texture_cache && texture_mode != static_cast<u8>(BatchTextureMode::Disabled))
  {
    g_gpu_device->SetTextureSampler(0, texture ? texture->texture : m_vram_read_texture.get(),
                                    g_gpu_device->GetNearestSampler());
  }

  GL_INS_FMT("Texture mode: {}", s_batch_texture_modes[texture_mode]);
  GL_INS_FMT("Transparency mode: {}", s_transparency_modes[static_cast<u8>(m_batch.transparency_mode)]);
  GL_INS_FMT("Render mode: {}", s_batch_render_modes[static_cast<u8>(render_mode)]);
  GL_INS_FMT("Mask bit test: {}", m_batch.check_mask_before_draw);
  GL_INS_FMT("Interlacing: {}", m_batch.check_mask_before_draw);

  // Activating ROV?
  if (render_mode == BatchRenderMode::ShaderBlend)
  {
    if (m_use_rov_for_shader_blend)
    {
      if (!m_rov_active)
      {
        GL_INS("Activating ROV.");
        m_rov_active = true;
        SetVRAMRenderTarget();
      }

      g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
    }
    else if (m_supports_framebuffer_fetch)
    {
      // No barriers needed for FBFetch.
      g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
    }
    else
    {
      // Barriers. Yucky.
      g_gpu_device->DrawIndexedWithBarrier(num_indices, base_index, base_vertex, GPUDevice::DrawBarrier::Full);
    }
  }
  else
  {
    g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
  }
}

ALWAYS_INLINE_RELEASE void GPU_HW::ComputeUVPartialDerivatives(const BatchVertex* vertices, float* dudx, float* dudy,
                                                               float* dvdx, float* dvdy, float* xy_area, s32* uv_area)
{
  const float v01x = vertices[1].x - vertices[0].x;
  const float v01y = vertices[1].y - vertices[0].y;
  const float v12x = vertices[2].x - vertices[1].x;
  const float v12y = vertices[2].y - vertices[1].y;
  const float v23x = vertices[0].x - vertices[2].x;
  const float v23y = vertices[0].y - vertices[2].y;
  const float v0u = static_cast<float>(vertices[0].u);
  const float v0v = static_cast<float>(vertices[0].v);
  const float v1u = static_cast<float>(vertices[1].u);
  const float v1v = static_cast<float>(vertices[1].v);
  const float v2u = static_cast<float>(vertices[2].u);
  const float v2v = static_cast<float>(vertices[2].v);
  *dudx = -v01y * v2u - v12y * v0u - v23y * v1u;
  *dvdx = -v01y * v2v - v12y * v0v - v23y * v1v;
  *dudy = v01x * v2u + v12x * v0u + v23x * v1u;
  *dvdy = v01x * v2v + v12x * v0v + v23x * v1v;
  *xy_area = v12x * v23y - v12y * v23x;
  *uv_area = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
             (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);
}

ALWAYS_INLINE_RELEASE void GPU_HW::HandleFlippedQuadTextureCoordinates(const GPUBackendDrawCommand* cmd,
                                                                       BatchVertex* vertices)
{
  // Hack for Wild Arms 2: The player sprite is drawn one line at a time with a quad, but the bottom V coordinates
  // are set to a large distance from the top V coordinate. When upscaling, this means that the coordinate is
  // interpolated between these two values, result in out-of-bounds sampling. At native, it's fine, because at the
  // top of the primitive, no amount is added to the coordinates. So, in this case, just set all coordinates to the
  // same value, from the first vertex, ensuring no interpolation occurs. Gate it based on the Y distance being one
  // pixel, limiting the risk of false positives.
  if (m_line_detect_mode == GPULineDetectMode::Quads &&
      (std::max(vertices[0].y, std::max(vertices[1].y, std::max(vertices[2].y, vertices[3].y))) -
       std::min(vertices[0].y, std::min(vertices[1].y, std::min(vertices[2].y, vertices[3].y)))) == 1.0f) [[unlikely]]
  {
    GL_INS_FMT("HLineQuad detected at [{},{}={},{} {},{}={},{} {},{}={},{} {},{}={},{}", vertices[0].x, vertices[0].y,
               vertices[0].u, vertices[0].v, vertices[1].x, vertices[1].y, vertices[1].u, vertices[1].v, vertices[2].x,
               vertices[2].y, vertices[2].u, vertices[2].v, vertices[3].x, vertices[3].y, vertices[3].u, vertices[3].v);
    vertices[1].v = vertices[0].v;
    vertices[2].v = vertices[0].v;
    vertices[3].v = vertices[0].v;
  }

  // Handle interpolation differences between PC GPUs and the PSX GPU. The first pixel on each span/scanline is given
  // the initial U/V coordinate without any further interpolation on the PSX GPU, in contrast to PC GPUs. This results
  // in oversampling on the right edge, so compensate by offsetting the left (right in texture space) UV.
  alignas(VECTOR_ALIGNMENT) float pd[4];
  float xy_area;
  s32 uv_area;
  ComputeUVPartialDerivatives(vertices, &pd[0], &pd[1], &pd[2], &pd[3], &xy_area, &uv_area);
  if (xy_area == 0.0f || uv_area == 0)
    return;

  const GSVector4 pd_area = GSVector4::load<true>(pd) / GSVector4(xy_area);
  const GSVector4 neg_pd = (pd_area < GSVector4::zero());
  const GSVector4 zero_pd = (pd_area == GSVector4::zero());
  const int mask = (neg_pd.mask() | (zero_pd.mask() << 4));

  // Addressing the 8-bit status code above.
  static constexpr int NEG_DUDX = 0x1;
  static constexpr int NEG_DUDY = 0x2;
  static constexpr int NEG_DVDX = 0x4;
  static constexpr int NEG_DVDY = 0x8;
  static constexpr int ZERO_DUDX = 0x10;
  static constexpr int ZERO_DUDY = 0x20;
  static constexpr int ZERO_DVDX = 0x40;
  static constexpr int ZERO_DVDY = 0x80;

  // Flipped horizontal sprites: negative dudx+zero dudy or negative dudy+zero dudx.
  if ((mask & (NEG_DUDX | ZERO_DUDY)) == (NEG_DUDX | ZERO_DUDY) ||
      (mask & (NEG_DUDY | ZERO_DUDX)) == (NEG_DUDY | ZERO_DUDX))
  {
    GL_INS_FMT("Horizontal flipped sprite detected at {},{}", vertices[0].x, vertices[0].y);
    vertices[0].u++;
    vertices[1].u++;
    vertices[2].u++;
    vertices[3].u++;
  }

  // Flipped vertical sprites: negative dvdx+zero dvdy or negative dvdy+zero dvdx.
  if ((mask & (NEG_DVDX | ZERO_DVDY)) == (NEG_DVDX | ZERO_DVDY) ||
      (mask & (NEG_DVDY | ZERO_DVDX)) == (NEG_DVDY | ZERO_DVDX))
  {
    GL_INS_FMT("Vertical flipped sprite detected at {},{}", vertices[0].x, vertices[0].y);
    vertices[0].v++;
    vertices[1].v++;
    vertices[2].v++;
    vertices[3].v++;
  }

  // 2D polygons should have zero change in V on the X axis, and vice versa.
  if (m_allow_sprite_mode)
  {
    const bool is_sprite = (mask & (ZERO_DVDX | ZERO_DUDY)) == (ZERO_DVDX | ZERO_DUDY);
    SetBatchSpriteMode(cmd, is_sprite);
  }
}

bool GPU_HW::IsPossibleSpritePolygon(const BatchVertex* vertices) const
{
  float dudx, dudy, dvdx, dvdy, xy_area;
  s32 uv_area;
  ComputeUVPartialDerivatives(vertices, &dudx, &dudy, &dvdx, &dvdy, &xy_area, &uv_area);
  if (xy_area == 0.0f || uv_area == 0)
    return m_batch.sprite_mode;

  // Could vectorize this, but it's not really worth it as we're only checking two partial derivatives.
  const float rcp_xy_area = 1.0f / xy_area;
  const bool zero_dudy = ((dudy * rcp_xy_area) == 0.0f);
  const bool zero_dvdx = ((dvdx * rcp_xy_area) == 0.0f);
  return (zero_dudy && zero_dvdx);
}

ALWAYS_INLINE_RELEASE bool GPU_HW::ExpandLineTriangles(BatchVertex* vertices)
{
  // Line expansion inspired by beetle-psx.
  BatchVertex *vshort, *vlong;
  bool vertical, horizontal;

  if (m_line_detect_mode == GPULineDetectMode::BasicTriangles)
  {
    // Given a tall/one-pixel-wide triangle, determine which vertex is the corner with axis-aligned edges.
    BatchVertex* vcorner;
    if (vertices[0].u == vertices[1].u && vertices[0].v == vertices[1].v)
    {
      // A,B,C
      vcorner = &vertices[0];
      vshort = &vertices[1];
      vlong = &vertices[2];
    }
    else if (vertices[1].u == vertices[2].u && vertices[1].v == vertices[2].v)
    {
      // B,C,A
      vcorner = &vertices[1];
      vshort = &vertices[2];
      vlong = &vertices[0];
    }
    else if (vertices[2].u == vertices[0].u && vertices[2].v == vertices[0].v)
    {
      // C,A,B
      vcorner = &vertices[2];
      vshort = &vertices[0];
      vlong = &vertices[1];
    }
    else
    {
      return false;
    }

    // Determine line direction. Vertical lines will have a width of 1, horizontal lines a height of 1.
    vertical = ((vcorner->y == vshort->y) && (std::abs(vcorner->x - vshort->x) == 1.0f));
    horizontal = ((vcorner->x == vshort->x) && (std::abs(vcorner->y - vshort->y) == 1.0f));
    if (vertical)
    {
      // Line should be vertical. Make sure the triangle is actually a right angle.
      if (vshort->x == vlong->x)
        std::swap(vshort, vcorner);
      else if (vcorner->x != vlong->x)
        return false;

      GL_INS_FMT("Vertical line from Y={} to {}", vcorner->y, vlong->y);
    }
    else if (horizontal)
    {
      // Line should be horizontal. Make sure the triangle is actually a right angle.
      if (vshort->y == vlong->y)
        std::swap(vshort, vcorner);
      else if (vcorner->y != vlong->y)
        return false;

      GL_INS_FMT("Horizontal line from X={} to {}", vcorner->x, vlong->x);
    }
    else
    {
      // Not a line-like triangle.
      return false;
    }

    // We could adjust the short texture coordinate to +1 from its original position, rather than leaving it the same.
    // However, since the texture is unlikely to be a higher resolution than the one-wide triangle, there would be no
    // benefit in doing so.
  }
  else
  {
    DebugAssert(m_line_detect_mode == GPULineDetectMode::AggressiveTriangles);

    // Find direction of line based on horizontal position.
    BatchVertex *va, *vb, *vc;
    if (vertices[0].x == vertices[1].x)
    {
      va = &vertices[0];
      vb = &vertices[1];
      vc = &vertices[2];
    }
    else if (vertices[1].x == vertices[2].x)
    {
      va = &vertices[1];
      vb = &vertices[2];
      vc = &vertices[0];
    }
    else if (vertices[2].x == vertices[0].x)
    {
      va = &vertices[2];
      vb = &vertices[0];
      vc = &vertices[1];
    }
    else
    {
      return false;
    }

    // Determine line direction. Vertical lines will have a width of 1, horizontal lines a height of 1.
    vertical = (std::abs(va->x - vc->x) == 1.0f);
    horizontal = (std::abs(va->y - vb->y) == 1.0f);
    if (!vertical && !horizontal)
      return false;

    // Determine which vertex is the right angle, based on the vertical position.
    const BatchVertex* vcorner;
    if (va->y == vc->y)
      vcorner = va;
    else if (vb->y == vc->y)
      vcorner = vb;
    else
      return false;

    // Find short/long edge of the triangle.
    BatchVertex* vother = ((vcorner == va) ? vb : va);
    vshort = horizontal ? vother : vc;
    vlong = vertical ? vother : vc;

    // Dark Forces draws its gun sprite vertically, but rotated compared to the sprite date in VRAM.
    // Therefore the difference in V should be ignored.
    vshort->u = vcorner->u;
    vshort->v = vcorner->v;
  }

  // Need to write the 4th vertex.
  DebugAssert(m_batch_vertex_space >= 1);
  BatchVertex* last = &(vertices[3] = *vlong);
  last->x = vertical ? vshort->x : vlong->x;
  last->y = horizontal ? vshort->y : vlong->y;

  // Generate indices.
  const u32 base_vertex = m_batch_vertex_count;
  DebugAssert(m_batch_index_space >= 6);
  *(m_batch_index_ptr++) = Truncate16(base_vertex);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + (vshort - vertices));
  *(m_batch_index_ptr++) = Truncate16(base_vertex + (vlong - vertices));
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 3);
  m_batch_index_count += 6;
  m_batch_index_space -= 6;

  // Upload vertices.
  DebugAssert(m_batch_vertex_space >= 4);
  std::memcpy(m_batch_vertex_ptr, vertices, sizeof(BatchVertex) * 4);
  m_batch_vertex_ptr[0].z = m_batch_vertex_ptr[1].z = m_batch_vertex_ptr[2].z = m_batch_vertex_ptr[3].z =
    GetCurrentNormalizedVertexDepth();
  m_batch_vertex_ptr += 4;
  m_batch_vertex_count += 4;
  m_batch_vertex_space -= 4;
  return true;
}

void GPU_HW::ComputePolygonUVLimits(const GPUBackendDrawCommand* cmd, BatchVertex* vertices, u32 num_vertices)
{
  DebugAssert(num_vertices == 3 || num_vertices == 4);

  GSVector2i v0 = GSVector2i::load32(&vertices[0].u);
  GSVector2i v1 = GSVector2i::load32(&vertices[1].u);
  GSVector2i v2 = GSVector2i::load32(&vertices[2].u);
  GSVector2i v3;
  GSVector2i min = v0.min_u16(v1).min_u16(v2);
  GSVector2i max = v0.max_u16(v1).max_u16(v2);
  if (num_vertices == 4)
  {
    v3 = GSVector2i::load32(&vertices[3].u);
    min = min.min_u16(v3);
    max = max.max_u16(v3);
  }

  u32 min_u = min.extract16<0>();
  u32 min_v = min.extract16<1>();
  u32 max_u = max.extract16<0>();
  u32 max_v = max.extract16<1>();
  max_u = (min_u != max_u) ? (max_u - 1) : max_u;
  max_v = (min_v != max_v) ? (max_v - 1) : max_v;

  for (u32 i = 0; i < num_vertices; i++)
    vertices[i].SetUVLimits(min_u, max_u, min_v, max_v);

  if (ShouldCheckForTexPageOverlap())
    CheckForTexPageOverlap(cmd, GSVector4i(min).upl32(GSVector4i(max)).u16to32());
}

void GPU_HW::SetBatchDepthBuffer(const GPUBackendDrawCommand* cmd, bool enabled)
{
  if (m_batch.use_depth_buffer == enabled)
    return;

  if (m_batch_index_count > 0)
  {
    FlushRender();
    EnsureVertexBufferSpaceForCommand(cmd);
  }

  m_batch.use_depth_buffer = enabled;
}

void GPU_HW::CheckForDepthClear(const GPUBackendDrawCommand* cmd, float average_z)
{
  if ((average_z - m_last_depth_z) >= g_gpu_settings.gpu_pgxp_depth_clear_threshold)
  {
    GL_INS_FMT("Clear depth buffer avg={} last={} threshold={}", average_z * static_cast<float>(GTE::MAX_Z),
               m_last_depth_z * static_cast<float>(GTE::MAX_Z),
               g_gpu_settings.gpu_pgxp_depth_clear_threshold * static_cast<float>(GTE::MAX_Z));

    FlushRender();
    CopyAndClearDepthBuffer(true);
    EnsureVertexBufferSpaceForCommand(cmd);
  }

  m_last_depth_z = average_z;
}

void GPU_HW::SetBatchSpriteMode(const GPUBackendDrawCommand* cmd, bool enabled)
{
  if (m_batch.sprite_mode == enabled)
    return;

  if (m_batch_index_count > 0)
  {
    FlushRender();
    EnsureVertexBufferSpaceForCommand(cmd);
  }

  GL_INS_FMT("Sprite mode is now {}", enabled ? "ON" : "OFF");

  m_batch.sprite_mode = enabled;
}

void GPU_HW::DrawLine(const GPUBackendDrawLineCommand* cmd)
{
  PrepareDraw(cmd);
  SetBatchDepthBuffer(cmd, false);

  const u32 num_vertices = cmd->num_vertices;
  DebugAssert(m_batch_vertex_space >= (num_vertices * 4) && m_batch_index_space >= (num_vertices * 6));

  for (u32 i = 0; i < num_vertices; i += 2)
  {
    const GSVector2i start_pos = GSVector2i::load<true>(&cmd->vertices[i].x);
    const u32 start_color = cmd->vertices[i].color;
    const GSVector2i end_pos = GSVector2i::load<true>(&cmd->vertices[i + 1].x);
    const u32 end_color = cmd->vertices[i + 1].color;

    const GSVector4i bounds = GSVector4i::xyxy(start_pos, end_pos);
    const GSVector4i rect =
      GSVector4i::xyxy(start_pos.min_s32(end_pos), start_pos.max_s32(end_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);
    DebugAssert(rect.width() <= MAX_PRIMITIVE_WIDTH && rect.height() <= MAX_PRIMITIVE_HEIGHT);
    if (clamped_rect.rempty())
    {
      GL_INS_FMT("Culling off-screen line {} => {}", start_pos, end_pos);
      continue;
    }

    AddDrawnRectangle(clamped_rect);
    DrawLine(cmd, GSVector4(bounds), start_color, end_color, 1.0f, 1.0f);
  }

  if (ShouldDrawWithSoftwareRenderer())
  {
    const GPU_SW_Rasterizer::DrawLineFunction DrawFunction =
      GPU_SW_Rasterizer::GetDrawLineFunction(cmd->shading_enable, cmd->transparency_enable);

    for (u32 i = 0; i < num_vertices; i += 2)
      DrawFunction(cmd, &cmd->vertices[i], &cmd->vertices[i + 1]);
  }
}

void GPU_HW::DrawPreciseLine(const GPUBackendDrawPreciseLineCommand* cmd)
{
  PrepareDraw(cmd);

  const bool use_depth = m_pgxp_depth_buffer && cmd->valid_w;
  SetBatchDepthBuffer(cmd, use_depth);

  const u32 num_vertices = cmd->num_vertices;
  DebugAssert(m_batch_vertex_space >= (num_vertices * 4) && m_batch_index_space >= (num_vertices * 6));

  for (u32 i = 0; i < num_vertices; i += 2)
  {
    float start_depth, end_depth;
    if (use_depth)
    {
      start_depth = cmd->vertices[i].w;
      end_depth = cmd->vertices[i + 1].w;

      const float average_z = ComputePolygonAverageZ(start_depth, end_depth);
      CheckForDepthClear(cmd, average_z);
    }
    else
    {
      start_depth = 1.0f;
      end_depth = 1.0f;
    }

    const GSVector2 start_pos = GSVector2::load<true>(&cmd->vertices[i].x);
    const u32 start_color = cmd->vertices[i].color;
    const GSVector2 end_pos = GSVector2::load<true>(&cmd->vertices[i + 1].x);
    const u32 end_color = cmd->vertices[i + 1].color;

    const GSVector4 bounds = GSVector4::xyxy(start_pos, end_pos);
    const GSVector4i rect =
      GSVector4i(GSVector4::xyxy(start_pos.min(end_pos), start_pos.max(end_pos))).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);
    if (clamped_rect.rempty())
    {
      GL_INS_FMT("Culling off-screen line {} => {}", start_pos, end_pos);
      continue;
    }

    AddDrawnRectangle(clamped_rect);
    DrawLine(cmd, bounds, start_color, end_color, start_depth, end_depth);
  }

  if (ShouldDrawWithSoftwareRenderer())
  {
    const GPU_SW_Rasterizer::DrawLineFunction DrawFunction =
      GPU_SW_Rasterizer::GetDrawLineFunction(cmd->shading_enable, cmd->transparency_enable);

    for (u32 i = 0; i < cmd->num_vertices; i += 2)
    {
      const GPUBackendDrawPreciseLineCommand::Vertex& RESTRICT start = cmd->vertices[i];
      const GPUBackendDrawPreciseLineCommand::Vertex& RESTRICT end = cmd->vertices[i + 1];
      const GPUBackendDrawLineCommand::Vertex vertices[2] = {
        {.x = start.native_x, .y = start.native_y, .color = start.color},
        {.x = end.native_x, .y = end.native_y, .color = end.color},
      };

      DrawFunction(cmd, &vertices[0], &vertices[1]);
    }
  }
}

void GPU_HW::DrawLine(const GPUBackendDrawCommand* cmd, const GSVector4 bounds, u32 col0, u32 col1, float depth0,
                      float depth1)
{
  DebugAssert(m_batch_vertex_space >= 4 && m_batch_index_space >= 6);

  if (ShouldTruncate32To16(cmd))
  {
    col0 = Truncate32To16(col0);
    col1 = Truncate32To16(col1);
  }

  const float x0 = bounds.x;
  const float y0 = bounds.y;
  const float x1 = bounds.z;
  const float y1 = bounds.w;

  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float mask_depth = GetCurrentNormalizedVertexDepth();
  if (dx == 0.0f && dy == 0.0f)
  {
    // Degenerate, render a point.
    (m_batch_vertex_ptr++)->Set(x0, y0, mask_depth, depth0, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x0 + 1.0f, y0, mask_depth, depth0, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x1, y1 + 1.0f, mask_depth, depth1, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x1 + 1.0f, y1 + 1.0f, mask_depth, depth1, col0, 0, 0, 0);
  }
  else
  {
    const float abs_dx = std::fabs(dx);
    const float abs_dy = std::fabs(dy);
    float fill_dx, fill_dy;
    float pad_x0 = 0.0f;
    float pad_x1 = 0.0f;
    float pad_y0 = 0.0f;
    float pad_y1 = 0.0f;

    // Check for vertical or horizontal major lines.
    // When expanding to a rect, do so in the appropriate direction.
    // FIXME: This scheme seems to kinda work, but it seems very hard to find a method
    // that looks perfect on every game.
    // Vagrant Story speech bubbles are a very good test case here!
    if (abs_dx > abs_dy)
    {
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      const float dydk = dy / abs_dx;

      if (dx > 0.0f)
      {
        // Right
        pad_x1 = 1.0f;
        pad_y1 = dydk;
      }
      else
      {
        // Left
        pad_x0 = 1.0f;
        pad_y0 = -dydk;
      }
    }
    else
    {
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      const float dxdk = dx / abs_dy;

      if (dy > 0.0f)
      {
        // Down
        pad_y1 = 1.0f;
        pad_x1 = dxdk;
      }
      else
      {
        // Up
        pad_y0 = 1.0f;
        pad_x0 = -dxdk;
      }
    }

    const float ox0 = x0 + pad_x0;
    const float oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1;
    const float oy1 = y1 + pad_y1;

    (m_batch_vertex_ptr++)->Set(ox0, oy0, mask_depth, depth0, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox0 + fill_dx, oy0 + fill_dy, mask_depth, depth0, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox1, oy1, mask_depth, depth1, col1, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox1 + fill_dx, oy1 + fill_dy, mask_depth, depth1, col1, 0, 0, 0);
  }

  const u32 start_index = m_batch_vertex_count;
  m_batch_vertex_count += 4;
  m_batch_vertex_space -= 4;

  *(m_batch_index_ptr++) = Truncate16(start_index + 0);
  *(m_batch_index_ptr++) = Truncate16(start_index + 1);
  *(m_batch_index_ptr++) = Truncate16(start_index + 2);
  *(m_batch_index_ptr++) = Truncate16(start_index + 3);
  *(m_batch_index_ptr++) = Truncate16(start_index + 2);
  *(m_batch_index_ptr++) = Truncate16(start_index + 1);
  m_batch_index_count += 6;
  m_batch_index_space -= 6;
}

void GPU_HW::DrawSprite(const GPUBackendDrawRectangleCommand* cmd)
{
  // Treat non-textured sprite draws as fills, so we don't break the TC on framebuffer clears.
  if (m_use_texture_cache && !cmd->transparency_enable && !cmd->shading_enable && !cmd->texture_enable && cmd->x >= 0 &&
      cmd->y >= 0 && cmd->width >= TEXTURE_PAGE_WIDTH && cmd->height >= TEXTURE_PAGE_HEIGHT &&
      (static_cast<u32>(cmd->x) + cmd->width) <= VRAM_WIDTH && (static_cast<u32>(cmd->y) + cmd->height) <= VRAM_HEIGHT)
  {
    FillVRAM(cmd->x, cmd->y, cmd->width, cmd->height, cmd->color, cmd->interlaced_rendering, cmd->active_line_lsb);
    return;
  }

  const GSVector2i pos = GSVector2i::load<true>(&cmd->x);
  const GSVector2i size = GSVector2i::load<true>(&cmd->width).u16to32();
  const GSVector4i rect = GSVector4i::xyxy(pos, pos.add32(size));
  const GSVector4i clamped_rect = m_clamped_drawing_area.rintersect(rect);
  if (clamped_rect.rempty())
  {
    GL_INS_FMT("Culling off-screen sprite {}", rect);
    return;
  }

  PrepareDraw(cmd);
  SetBatchDepthBuffer(cmd, false);
  SetBatchSpriteMode(cmd, m_allow_sprite_mode);
  DebugAssert(m_batch_vertex_space >= MAX_VERTICES_FOR_RECTANGLE && m_batch_index_space >= MAX_VERTICES_FOR_RECTANGLE);

  const s32 pos_x = cmd->x;
  const s32 pos_y = cmd->y;
  const u32 texpage = m_draw_mode.bits;
  const u32 color = (cmd->texture_enable && cmd->raw_texture_enable) ?
                      UINT32_C(0x00808080) :
                      (ShouldTruncate32To16(cmd) ? Truncate32To16(cmd->color) : cmd->color);
  const float depth = GetCurrentNormalizedVertexDepth();
  const u32 orig_tex_left = ZeroExtend32(Truncate8(cmd->texcoord));
  const u32 orig_tex_top = ZeroExtend32(cmd->texcoord) >> 8;
  const u32 rectangle_width = cmd->width;
  const u32 rectangle_height = cmd->height;

  // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
  u32 tex_top = orig_tex_top;
  for (u32 y_offset = 0; y_offset < rectangle_height;)
  {
    const s32 quad_height = std::min(rectangle_height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
    const float quad_start_y = static_cast<float>(pos_y + static_cast<s32>(y_offset));
    const float quad_end_y = quad_start_y + static_cast<float>(quad_height);
    const u32 tex_bottom = tex_top + quad_height;

    u32 tex_left = orig_tex_left;
    for (u32 x_offset = 0; x_offset < rectangle_width;)
    {
      const s32 quad_width = std::min(rectangle_width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
      const float quad_start_x = static_cast<float>(pos_x + static_cast<s32>(x_offset));
      const float quad_end_x = quad_start_x + static_cast<float>(quad_width);
      const u32 tex_right = tex_left + quad_width;
      const u32 uv_limits = BatchVertex::PackUVLimits(tex_left, tex_right - 1, tex_top, tex_bottom - 1);

      if (cmd->texture_enable && ShouldCheckForTexPageOverlap())
      {
        CheckForTexPageOverlap(cmd, GSVector4i(static_cast<s32>(tex_left), static_cast<s32>(tex_top),
                                               static_cast<s32>(tex_right) - 1, static_cast<s32>(tex_bottom) - 1));
      }

      const u32 base_vertex = m_batch_vertex_count;
      (m_batch_vertex_ptr++)
        ->Set(quad_start_x, quad_start_y, depth, 1.0f, color, texpage, Truncate16(tex_left), Truncate16(tex_top),
              uv_limits);
      (m_batch_vertex_ptr++)
        ->Set(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, Truncate16(tex_right), Truncate16(tex_top),
              uv_limits);
      (m_batch_vertex_ptr++)
        ->Set(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, Truncate16(tex_left), Truncate16(tex_bottom),
              uv_limits);
      (m_batch_vertex_ptr++)
        ->Set(quad_end_x, quad_end_y, depth, 1.0f, color, texpage, Truncate16(tex_right), Truncate16(tex_bottom),
              uv_limits);
      m_batch_vertex_count += 4;
      m_batch_vertex_space -= 4;

      *(m_batch_index_ptr++) = Truncate16(base_vertex + 0);
      *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
      *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
      *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
      *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
      *(m_batch_index_ptr++) = Truncate16(base_vertex + 3);
      m_batch_index_count += 6;
      m_batch_index_space -= 6;

      x_offset += quad_width;
      tex_left = 0;
    }

    y_offset += quad_height;
    tex_top = 0;
  }

  AddDrawnRectangle(clamped_rect);

  if (ShouldDrawWithSoftwareRenderer())
  {
    const GPU_SW_Rasterizer::DrawRectangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawRectangleFunction(
      cmd->texture_enable, cmd->raw_texture_enable, cmd->transparency_enable);
    DrawFunction(cmd);
  }
}

void GPU_HW::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
  // TODO: This could write directly to the mapped GPU pointer. But watch out for the reads below.
  const bool raw_texture = (cmd->texture_enable && cmd->raw_texture_enable);
  const u32 texpage = ZeroExtend32(cmd->draw_mode.bits) | (ZeroExtend32(cmd->palette.bits) << 16);
  std::array<BatchVertex, 4> vertices;
  u32 num_vertices = cmd->num_vertices;
  for (u32 i = 0; i < num_vertices; i++)
  {
    const GPUBackendDrawPolygonCommand::Vertex& vert = cmd->vertices[i];
    const GSVector2 vert_pos = GSVector2(GSVector2i::load<true>(&vert.x));
    vertices[i].Set(vert_pos.x, vert_pos.y, 0.0f, 1.0f, raw_texture ? UINT32_C(0x00808080) : vert.color, texpage,
                    vert.texcoord, 0xFFFF0000u);
  }

  GSVector4i clamped_draw_rect_012, clamped_draw_rect_123;
  if (BeginPolygonDraw(cmd, vertices, num_vertices, clamped_draw_rect_012, clamped_draw_rect_123))
  {
    SetBatchDepthBuffer(cmd, false);

    FinishPolygonDraw(cmd, vertices, num_vertices, false, false, clamped_draw_rect_012, clamped_draw_rect_123);
  }

  if (ShouldDrawWithSoftwareRenderer())
  {
    const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
      cmd->shading_enable, cmd->texture_enable, cmd->raw_texture_enable, cmd->transparency_enable);
    DrawFunction(cmd, &cmd->vertices[0], &cmd->vertices[1], &cmd->vertices[2]);
    if (cmd->num_vertices > 3)
      DrawFunction(cmd, &cmd->vertices[2], &cmd->vertices[1], &cmd->vertices[3]);
  }
}

void GPU_HW::DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd)
{
  // TODO: This could write directly to the mapped GPU pointer. But watch out for the reads below.
  const bool raw_texture = (cmd->texture_enable && cmd->raw_texture_enable);
  const u32 texpage = ZeroExtend32(cmd->draw_mode.bits) | (ZeroExtend32(cmd->palette.bits) << 16);
  std::array<BatchVertex, 4> vertices;
  u32 num_vertices = cmd->num_vertices;
  for (u32 i = 0; i < num_vertices; i++)
  {
    const GPUBackendDrawPrecisePolygonCommand::Vertex& vert = cmd->vertices[i];
    vertices[i].Set(vert.x, vert.y, 0.0f, vert.w, raw_texture ? UINT32_C(0x00808080) : vert.color, texpage,
                    vert.texcoord, 0xFFFF0000u);
  }

  GSVector4i clamped_draw_rect_012, clamped_draw_rect_123;
  if (BeginPolygonDraw(cmd, vertices, num_vertices, clamped_draw_rect_012, clamped_draw_rect_123))
  {
    // Use PGXP to exclude primitives that are definitely 3D.
    const bool is_3d = (vertices[0].w != vertices[1].w || vertices[0].w != vertices[2].w ||
                        (num_vertices == 4 && vertices[0].w != vertices[3].w));
    const bool use_depth =
      m_pgxp_depth_buffer && is_3d && (!cmd->transparency_enable || g_gpu_settings.gpu_pgxp_transparent_depth);
    SetBatchDepthBuffer(cmd, use_depth);
    if (use_depth)
    {
      const float average_z = (num_vertices == 4) ?
                                ComputePolygonAverageZ(vertices[0].w, vertices[1].w, vertices[2].w, vertices[3].w) :
                                ComputePolygonAverageZ(vertices[0].w, vertices[1].w, vertices[2].w);
      CheckForDepthClear(cmd, average_z);
    }

    FinishPolygonDraw(cmd, vertices, num_vertices, true, is_3d, clamped_draw_rect_012, clamped_draw_rect_123);
  }

  if (ShouldDrawWithSoftwareRenderer())
  {
    const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
      cmd->shading_enable, cmd->texture_enable, cmd->raw_texture_enable, cmd->transparency_enable);
    GPUBackendDrawPolygonCommand::Vertex sw_vertices[4];
    for (u32 i = 0; i < cmd->num_vertices; i++)
    {
      const GPUBackendDrawPrecisePolygonCommand::Vertex& src = cmd->vertices[i];
      sw_vertices[i] = GPUBackendDrawPolygonCommand::Vertex{
        .x = src.native_x, .y = src.native_y, .color = src.color, .texcoord = src.texcoord};
    }

    DrawFunction(cmd, &sw_vertices[0], &sw_vertices[1], &sw_vertices[2]);
    if (cmd->num_vertices > 3)
      DrawFunction(cmd, &sw_vertices[2], &sw_vertices[1], &sw_vertices[3]);
  }
}

ALWAYS_INLINE_RELEASE bool GPU_HW::BeginPolygonDraw(const GPUBackendDrawCommand* cmd,
                                                    std::array<BatchVertex, 4>& vertices, u32& num_vertices,
                                                    GSVector4i& clamped_draw_rect_012,
                                                    GSVector4i& clamped_draw_rect_123)
{
  GSVector2 v0f = GSVector2::load<true>(&vertices[0].x);
  GSVector2 v1f = GSVector2::load<true>(&vertices[1].x);
  GSVector2 v2f = GSVector2::load<true>(&vertices[2].x);
  GSVector2 min_pos_12 = v1f.min(v2f);
  GSVector2 max_pos_12 = v1f.max(v2f);
  GSVector4i draw_rect_012 =
    GSVector4i(GSVector4(min_pos_12.min(v0f)).upld(GSVector4(max_pos_12.max(v0f)))).add32(GSVector4i::cxpr(0, 0, 1, 1));
  clamped_draw_rect_012 = draw_rect_012.rintersect(m_clamped_drawing_area);
  bool first_tri_culled = clamped_draw_rect_012.rempty();
  if (first_tri_culled)
  {
    // What is this monstrosity? Final Fantasy VIII relies on X coordinates being truncated during scanline drawing,
    // with negative coordinates becoming positive and vice versa. Fortunately the bits that we need are consistent
    // across the entire polygon, so we can get away with truncating the vertices. However, we can't do this to all
    // vertices, because other game's vertices break in various ways. For example, +1024 becomes -1024, which is a
    // valid vertex position as the ending coordinate is exclusive. Therefore, 1024 is never truncated, only 1023.
    // Luckily, FF8's vertices get culled as they do not intersect with the clip rectangle, so we can do this fixup
    // only when culled, and everything seems happy.

    const auto truncate_pos = [](const GSVector4 pos) {
      // See TruncateGPUVertexPosition().
      GSVector4i ipos = GSVector4i(pos);
      const GSVector4 fdiff = pos - GSVector4(ipos);
      ipos = ipos.sll32<21>().sra32<21>();
      return GSVector4(ipos) + fdiff;
    };

    const GSVector4 tv01f = truncate_pos(GSVector4::xyxy(v0f, v1f));
    const GSVector4 tv23f = truncate_pos(GSVector4::xyxy(v2f, GSVector2::load<true>(&vertices[3].x)));
    const GSVector2 tv0f = tv01f.xy();
    const GSVector2 tv1f = tv01f.zw();
    const GSVector2 tv2f = tv23f.xy();
    const GSVector2 tmin_pos_12 = tv1f.min(tv2f);
    const GSVector2 tmax_pos_12 = tv1f.max(tv2f);
    const GSVector4i tdraw_rect_012 =
      GSVector4i(GSVector4(tmin_pos_12.min(tv0f)).upld(GSVector4(tmax_pos_12.max(tv0f))))
        .add32(GSVector4i::cxpr(0, 0, 1, 1));
    first_tri_culled =
      (tdraw_rect_012.width() > MAX_PRIMITIVE_WIDTH || tdraw_rect_012.height() > MAX_PRIMITIVE_HEIGHT ||
       !tdraw_rect_012.rintersects(m_clamped_drawing_area));
    if (!first_tri_culled)
    {
      GSVector4::storel<true>(&vertices[0].x, tv01f);
      GSVector4::storeh<true>(&vertices[1].x, tv01f);
      GSVector4::storel<true>(&vertices[2].x, tv23f);
      if (num_vertices == 4)
        GSVector4::storeh<true>(&vertices[3].x, tv23f);

      GL_INS_FMT("Adjusted polygon from [{} {} {}] to [{} {} {}] due to coordinate truncation", v0f, v1f, v2f, tv0f,
                 tv1f, tv2f);

      v0f = tv0f;
      v1f = tv1f;
      v2f = tv2f;
      min_pos_12 = tmin_pos_12;
      max_pos_12 = tmax_pos_12;
    }
    else
    {
      GL_INS_FMT("Culling off-screen polygon: {},{} {},{} {},{}", vertices[0].x, vertices[0].y, vertices[1].y,
                 vertices[1].x, vertices[2].y, vertices[2].y);

      if (num_vertices != 4)
        return false;
    }
  }

  if (num_vertices == 4)
  {
    const GSVector2 v3f = GSVector2::load<true>(&vertices[3].x);
    const GSVector4i draw_rect_123 = GSVector4i(GSVector4(min_pos_12.min(v3f)).upld(GSVector4(max_pos_12.max(v3f))))
                                       .add32(GSVector4i::cxpr(0, 0, 1, 1));
    clamped_draw_rect_123 = draw_rect_123.rintersect(m_clamped_drawing_area);
    const bool second_tri_culled = clamped_draw_rect_123.rempty();
    if (second_tri_culled)
    {
      GL_INS_FMT("Culling off-screen polygon (quad second half): {},{} {},{} {},{}", vertices[2].x, vertices[2].y,
                 vertices[1].x, vertices[1].y, vertices[3].x, vertices[3].y);

      if (first_tri_culled)
      {
        // both parts culled
        return false;
      }

      // Remove second part of quad.
      // NOTE: Culling this way results in subtle differences with UV clamping, since the fourth vertex is no
      // longer considered in the range. This is mainly apparent when the UV gradient is zero. Seems like it
      // generally looks better this way, so I'm keeping it.
      num_vertices = 3;
    }
    else
    {
      // If first part was culled, move the second part to the first.
      if (first_tri_culled)
      {
        clamped_draw_rect_012 = clamped_draw_rect_123;
        std::memcpy(&vertices[0], &vertices[2], sizeof(BatchVertex));
        std::memcpy(&vertices[2], &vertices[3], sizeof(BatchVertex));
        num_vertices = 3;
      }
    }
  }

  if (ShouldTruncate32To16(cmd))
  {
    for (u32 i = 0; i < 4; i++)
      vertices[i].color = Truncate32To16(vertices[i].color);
  }

  PrepareDraw(cmd);
  return true;
}

ALWAYS_INLINE_RELEASE void GPU_HW::FinishPolygonDraw(const GPUBackendDrawCommand* cmd,
                                                     std::array<BatchVertex, 4>& vertices, u32 num_vertices,
                                                     bool is_precise, bool is_3d,
                                                     const GSVector4i clamped_draw_rect_012,
                                                     const GSVector4i clamped_draw_rect_123)
{
  // Use PGXP to exclude primitives that are definitely 3D.
  if (m_resolution_scale > 1 && !is_3d && cmd->quad_polygon)
    HandleFlippedQuadTextureCoordinates(cmd, vertices.data());
  else if (m_allow_sprite_mode)
    SetBatchSpriteMode(cmd, is_precise ? !is_3d : IsPossibleSpritePolygon(vertices.data()));

  if (cmd->texture_enable && m_compute_uv_range)
    ComputePolygonUVLimits(cmd, vertices.data(), num_vertices);

  AddDrawnRectangle(clamped_draw_rect_012);

  // Expand lines to triangles (Doom, Soul Blade, etc.)
  if (!cmd->quad_polygon && m_line_detect_mode >= GPULineDetectMode::BasicTriangles && !is_3d &&
      ExpandLineTriangles(vertices.data()))
  {
    return;
  }

  const u32 start_index = m_batch_vertex_count;
  DebugAssert(m_batch_index_space >= 3);
  *(m_batch_index_ptr++) = Truncate16(start_index);
  *(m_batch_index_ptr++) = Truncate16(start_index + 1);
  *(m_batch_index_ptr++) = Truncate16(start_index + 2);
  m_batch_index_count += 3;
  m_batch_index_space -= 3;

  // quads, use num_vertices here, because the first half might be culled
  if (num_vertices == 4)
  {
    AddDrawnRectangle(clamped_draw_rect_123);

    DebugAssert(m_batch_index_space >= 3);
    *(m_batch_index_ptr++) = Truncate16(start_index + 2);
    *(m_batch_index_ptr++) = Truncate16(start_index + 1);
    *(m_batch_index_ptr++) = Truncate16(start_index + 3);
    m_batch_index_count += 3;
    m_batch_index_space -= 3;

    // Fake depth must be written here rather than at vertex init time, because a flush could occur in between.
    DebugAssert(m_batch_vertex_space >= 4);
    std::memcpy(m_batch_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 4);
    m_batch_vertex_ptr[0].z = m_batch_vertex_ptr[1].z = m_batch_vertex_ptr[2].z = m_batch_vertex_ptr[3].z =
      GetCurrentNormalizedVertexDepth();
    m_batch_vertex_ptr += 4;
    m_batch_vertex_count += 4;
    m_batch_vertex_space -= 4;
  }
  else
  {
    DebugAssert(m_batch_vertex_space >= 3);
    std::memcpy(m_batch_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
    m_batch_vertex_ptr[0].z = m_batch_vertex_ptr[1].z = m_batch_vertex_ptr[2].z = GetCurrentNormalizedVertexDepth();
    m_batch_vertex_ptr += 3;
    m_batch_vertex_count += 3;
    m_batch_vertex_space -= 3;
  }
}

bool GPU_HW::BlitVRAMReplacementTexture(GPUTexture* tex, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  GL_SCOPE_FMT("BlitVRAMReplacementTexture() {}x{} to {},{} => {},{} ({}x{})", tex->GetWidth(), tex->GetHeight(), dst_x,
               dst_y, dst_x + width, dst_y + height, width, height);

  g_gpu_device->SetTextureSampler(0, tex, g_gpu_device->GetLinearSampler());
  g_gpu_device->SetPipeline(m_vram_write_replacement_pipeline.get());

  const GSVector4i rect(dst_x, dst_y, dst_x + width, dst_y + height);
  g_gpu_device->SetScissor(rect);
  DrawScreenQuad(rect, m_vram_texture->GetSizeVec(), GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), nullptr, 0);
  RestoreDeviceContext();
  return true;
}

ALWAYS_INLINE_RELEASE void GPU_HW::CheckForTexPageOverlap(const GPUBackendDrawCommand* cmd, GSVector4i uv_rect)
{
  DebugAssert((m_texpage_dirty != 0 || m_texture_dumping) && m_batch.texture_mode != BatchTextureMode::Disabled);

  if (m_texture_window_active)
  {
    const GSVector4i twin = GSVector4i::load<false>(m_batch_ubo_data.u_texture_window);
    uv_rect = ((uv_rect & twin.xyxy()) | twin.zwzw());

    // Min could be greater than max after applying window, correct for it.
    uv_rect = uv_rect.min_s32(uv_rect.zwzw()).max_s32(uv_rect.xyxy());
  }

  const GPUTextureMode tmode = m_draw_mode.mode_reg.texture_mode;
  const u32 xshift = (tmode >= GPUTextureMode::Direct16Bit) ? 0 : (2 - static_cast<u8>(tmode));
  const GSVector4i page_offset = GSVector4i::loadl<true>(m_current_texture_page_offset).xyxy();

  uv_rect = uv_rect.blend32<5>(uv_rect.srl32(xshift));   // shift only goes on the x
  uv_rect = uv_rect.add32(page_offset);                  // page offset
  uv_rect = uv_rect.add32(GSVector4i::cxpr(0, 0, 1, 1)); // make exclusive
  uv_rect = uv_rect.rintersect(VRAM_SIZE_RECT);          // clamp to vram bounds

  const GSVector4i new_uv_rect = m_current_uv_rect.runion(uv_rect);

  if (!m_current_uv_rect.eq(new_uv_rect))
  {
    m_current_uv_rect = new_uv_rect;

    bool update_drawn = false, update_written = false;
    if (m_texpage_dirty & TEXPAGE_DIRTY_PAGE_RECT)
    {
      DebugAssert(!(m_texpage_dirty & (TEXPAGE_DIRTY_DRAWN_RECT | TEXPAGE_DIRTY_WRITTEN_RECT)));
      DebugAssert(m_batch.texture_mode == BatchTextureMode::PageTexture && m_texture_cache_key.page < NUM_VRAM_PAGES);

      if (GPUTextureCache::AreSourcePagesDrawn(m_texture_cache_key, m_current_uv_rect))
      {
        // UVs intersect with drawn area, can't use TC
        if (m_batch_index_count > 0)
        {
          FlushRender();
          EnsureVertexBufferSpaceForCommand(cmd);
        }

        // We need to swap the dirty tracking over to drawn/written.
        const GSVector4i page_rect = GetTextureRect(m_texture_cache_key.page, m_texture_cache_key.mode);
        m_texpage_dirty = (m_vram_dirty_draw_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_DRAWN_RECT : 0) |
                          (m_vram_dirty_write_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_WRITTEN_RECT : 0);
        m_compute_uv_range = (ShouldCheckForTexPageOverlap() || m_clamp_uvs);
        m_batch.texture_mode = static_cast<BatchTextureMode>(m_draw_mode.mode_reg.texture_mode.GetValue());
      }
      else
      {
        // Page isn't drawn, we're done.
        return;
      }
    }
    if (m_texpage_dirty & TEXPAGE_DIRTY_DRAWN_RECT)
    {
      DebugAssert(!m_vram_dirty_draw_rect.eq(INVALID_RECT));
      update_drawn = m_current_uv_rect.rintersects(m_vram_dirty_draw_rect);
      if (update_drawn)
      {
        GL_INS_FMT("Updating VRAM cache due to UV {} intersection with dirty DRAW {}", m_current_uv_rect,
                   m_vram_dirty_draw_rect);
      }
    }
    if (m_texpage_dirty & TEXPAGE_DIRTY_WRITTEN_RECT)
    {
      DebugAssert(!m_vram_dirty_write_rect.eq(INVALID_RECT));
      update_written = m_current_uv_rect.rintersects(m_vram_dirty_write_rect);
      if (update_written)
      {
        GL_INS_FMT("Updating VRAM cache due to UV {} intersection with dirty WRITE {}", m_current_uv_rect,
                   m_vram_dirty_write_rect);
      }
    }

    if (update_drawn || update_written)
    {
      if (m_batch_index_count > 0)
      {
        FlushRender();
        EnsureVertexBufferSpaceForCommand(cmd);
      }

      UpdateVRAMReadTexture(update_drawn, update_written);
    }
  }
}

bool GPU_HW::ShouldCheckForTexPageOverlap() const
{
  return (m_texpage_dirty != 0);
}

ALWAYS_INLINE bool GPU_HW::IsFlushed() const
{
  return (m_batch_index_count == 0);
}

ALWAYS_INLINE_RELEASE bool GPU_HW::NeedsTwoPassRendering() const
{
  // We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
  // on a per-pixel basis, and the opaque pixels shouldn't be blended at all.

  return (m_batch.texture_mode != BatchTextureMode::Disabled &&
          (m_batch.transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ||
           (!m_supports_dual_source_blend && m_batch.transparency_mode != GPUTransparencyMode::Disabled)));
}

ALWAYS_INLINE_RELEASE bool GPU_HW::NeedsShaderBlending(GPUTransparencyMode transparency, BatchTextureMode texture_mode,
                                                       bool check_mask) const
{
  return (m_allow_shader_blend &&
          ((check_mask && !m_write_mask_as_depth) ||
           (transparency != GPUTransparencyMode::Disabled && m_prefer_shader_blend) ||
           (transparency == GPUTransparencyMode::BackgroundMinusForeground) ||
           (!m_supports_dual_source_blend && texture_mode != BatchTextureMode::Disabled &&
            (transparency != GPUTransparencyMode::Disabled || IsBlendedTextureFiltering(m_texture_filtering) ||
             IsBlendedTextureFiltering(m_sprite_texture_filtering)))));
}

void GPU_HW::EnsureVertexBufferSpace(u32 required_vertices, u32 required_indices)
{
  if (m_batch_vertex_ptr)
  {
    if (m_batch_vertex_space >= required_vertices && m_batch_index_space >= required_indices)
      return;

    FlushRender();
  }

  MapGPUBuffer(required_vertices, required_indices);
}

void GPU_HW::EnsureVertexBufferSpaceForCommand(const GPUBackendDrawCommand* cmd)
{
  u32 required_vertices;
  u32 required_indices;
  switch (cmd->type)
  {
    case GPUBackendCommandType::DrawPolygon:
    case GPUBackendCommandType::DrawPrecisePolygon:
      required_vertices = 4; // assume quad, in case of expansion
      required_indices = 6;
      break;
    case GPUBackendCommandType::DrawRectangle:
      required_vertices = MAX_VERTICES_FOR_RECTANGLE; // TODO: WRong
      required_indices = MAX_VERTICES_FOR_RECTANGLE;
      break;
    case GPUBackendCommandType::DrawLine:
    case GPUBackendCommandType::DrawPreciseLine:
    {
      // assume expansion
      const GPUBackendDrawLineCommand* lcmd = static_cast<const GPUBackendDrawLineCommand*>(cmd);
      required_vertices = lcmd->num_vertices * 4;
      required_indices = lcmd->num_vertices * 6;
    }
    break;

    default:
      UnreachableCode();
  }

  // can we fit these vertices in the current depth buffer range?
  if ((m_current_depth + required_vertices) > MAX_BATCH_VERTEX_COUNTER_IDS)
  {
    FlushRender();
    ResetBatchVertexDepth();
    MapGPUBuffer(required_vertices, required_indices);
    return;
  }

  EnsureVertexBufferSpace(required_vertices, required_indices);
}

void GPU_HW::ResetBatchVertexDepth()
{
  DEV_LOG("Resetting batch vertex depth");

  if (m_write_mask_as_depth)
    UpdateDepthBufferFromMaskBit();

  m_current_depth = 1;
}

ALWAYS_INLINE float GPU_HW::GetCurrentNormalizedVertexDepth() const
{
  return 1.0f - (static_cast<float>(m_current_depth) / 65535.0f);
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced_rendering, u8 active_line_lsb)
{
  FlushRender();
  DeactivateROV();

  GL_SCOPE_FMT("FillVRAM({},{} => {},{} ({}x{}) with 0x{:08X}", x, y, x + width, y + height, width, height, color);

  GL_INS_FMT("Dirty draw area before: {}", m_vram_dirty_draw_rect);

  const GSVector4i bounds = GetVRAMTransferBounds(x, y, width, height);

  // If TC is enabled, we have to update local memory.
  if (m_use_texture_cache && !interlaced_rendering)
  {
    AddWrittenRectangle(bounds);
    GPU_SW_Rasterizer::FillVRAM(x, y, width, height, color, false, 0);
  }
  else
  {
    AddUnclampedDrawnRectangle(bounds);
    if (ShouldDrawWithSoftwareRenderer())
      GPU_SW_Rasterizer::FillVRAM(x, y, width, height, color, interlaced_rendering, active_line_lsb);
  }

  GL_INS_FMT("Dirty draw area after: {}", m_vram_dirty_draw_rect);

  const bool is_oversized = (((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT));
  g_gpu_device->SetPipeline(m_vram_fill_pipelines[BoolToUInt8(is_oversized)][BoolToUInt8(interlaced_rendering)].get());

  struct VRAMFillUBOData
  {
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    std::array<float, 4> u_fill_color;
    u32 u_interlaced_displayed_field;
    u32 pad[3];
  };
  VRAMFillUBOData uniforms;
  uniforms.u_dst_x = (x % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_dst_y = (y % VRAM_HEIGHT) * m_resolution_scale;
  uniforms.u_end_x = ((x + width) % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_end_y = ((y + height) % VRAM_HEIGHT) * m_resolution_scale;
  // drop precision unless true colour is enabled
  uniforms.u_fill_color =
    GPUDevice::RGBA8ToFloat(m_true_color ? color : VRAMRGBA5551ToRGBA8888(VRAMRGBA8888ToRGBA5551(color)));
  uniforms.u_interlaced_displayed_field = active_line_lsb;
  std::memset(uniforms.pad, 0, sizeof(uniforms.pad));

  const GSVector4i scaled_bounds = bounds.mul32l(GSVector4i(m_resolution_scale));
  g_gpu_device->SetScissor(scaled_bounds);
  DrawScreenQuad(scaled_bounds, m_vram_texture->GetSizeVec(), GSVector4::zero(), &uniforms, sizeof(uniforms));

  RestoreDeviceContext();
}

void GPU_HW::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  GL_SCOPE_FMT("ReadVRAM({},{} => {},{} ({}x{})", x, y, x + width, y + height, width, height);

  if (ShouldDrawWithSoftwareRenderer())
  {
    GL_INS("VRAM is already up to date due to SW draws.");
    return;
  }

  DownloadVRAMFromGPU(x, y, width, height);
}

void GPU_HW::DownloadVRAMFromGPU(u32 x, u32 y, u32 width, u32 height)
{
  FlushRender();

  // TODO: Only read if it's in the drawn area

  // Get bounds with wrap-around handled.
  GSVector4i copy_rect = GetVRAMTransferBounds(x, y, width, height);

  // Has to be aligned to an even pixel for the download, due to 32-bit packing.
  if (copy_rect.left & 1)
    copy_rect.left--;
  if (copy_rect.right & 1)
    copy_rect.right++;

  DebugAssert((copy_rect.left % 2) == 0 && (copy_rect.width() % 2) == 0);
  const u32 encoded_left = copy_rect.left / 2;
  const u32 encoded_top = copy_rect.top;
  const u32 encoded_width = copy_rect.width() / 2;
  const u32 encoded_height = copy_rect.height();

  // Encode the 24-bit texture as 16-bit.
  const s32 uniforms[4] = {copy_rect.left, copy_rect.top, copy_rect.width(), copy_rect.height()};
  g_gpu_device->SetRenderTarget(m_vram_readback_texture.get());
  g_gpu_device->SetPipeline(m_vram_readback_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
  g_gpu_device->SetViewportAndScissor(0, 0, encoded_width, encoded_height);
  g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

  // Stage the readback and copy it into our shadow buffer.
  if (m_vram_readback_download_texture->IsImported())
  {
    // Fast path, read directly.
    m_vram_readback_download_texture->CopyFromTexture(encoded_left, encoded_top, m_vram_readback_texture.get(), 0, 0,
                                                      encoded_width, encoded_height, 0, 0, false);
    m_vram_readback_download_texture->Flush();
  }
  else
  {
    // Copy to staging buffer, then to VRAM.
    m_vram_readback_download_texture->CopyFromTexture(0, 0, m_vram_readback_texture.get(), 0, 0, encoded_width,
                                                      encoded_height, 0, 0, true);
    m_vram_readback_download_texture->ReadTexels(0, 0, encoded_width, encoded_height,
                                                 &g_vram[copy_rect.top * VRAM_WIDTH + copy_rect.left],
                                                 VRAM_WIDTH * sizeof(u16));
  }

  RestoreDeviceContext();
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  FlushRender();

  GL_SCOPE_FMT("UpdateVRAM({},{} => {},{} ({}x{})", x, y, x + width, y + height, width, height);

  // TODO: Handle wrapped transfers... break them up or something
  const GSVector4i bounds = GetVRAMTransferBounds(x, y, width, height);
  DebugAssert(bounds.right <= static_cast<s32>(VRAM_WIDTH) && bounds.bottom <= static_cast<s32>(VRAM_HEIGHT));
  AddWrittenRectangle(bounds);

  GPUTextureCache::WriteVRAM(x, y, width, height, data, set_mask, check_mask, bounds);

  if (check_mask)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
  else
  {
    // no point dumping things we can't replace, so put it after the mask check
    if (GPUTextureCache::ShouldDumpVRAMWrite(width, height))
      GPUTextureCache::DumpVRAMWrite(width, height, data);

    GPUTexture* rtex = GPUTextureCache::GetVRAMReplacement(width, height, data);
    if (rtex && BlitVRAMReplacementTexture(rtex, x * m_resolution_scale, y * m_resolution_scale,
                                           width * m_resolution_scale, height * m_resolution_scale))
    {
      return;
    }
  }

  UpdateVRAMOnGPU(x, y, width, height, data, sizeof(u16) * width, set_mask, check_mask, bounds);
}

void GPU_HW::UpdateVRAMOnGPU(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_pitch, bool set_mask,
                             bool check_mask, const GSVector4i bounds)
{
  DeactivateROV();

  GPUDevice::AutoRecycleTexture upload_texture;
  u32 map_index;

  if (!g_gpu_device->GetFeatures().texture_buffers)
  {
    map_index = 0;
    upload_texture =
      g_gpu_device->FetchAutoRecycleTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::R16U,
                                            GPUTexture::Flags::None, data, data_pitch);
    if (!upload_texture)
    {
      ERROR_LOG("Failed to get {}x{} upload texture. Things are gonna break.", width, height);
      return;
    }
  }
  else
  {
    const u32 num_pixels = width * height;
    const u32 dst_pitch = width * sizeof(u16);
    void* map = m_vram_upload_buffer->Map(num_pixels);
    map_index = m_vram_upload_buffer->GetCurrentPosition();
    StringUtil::StrideMemCpy(map, dst_pitch, data, data_pitch, dst_pitch, height);
    m_vram_upload_buffer->Unmap(num_pixels);
  }

  struct VRAMWriteUBOData
  {
    float u_dst_x;
    float u_dst_y;
    float u_end_x;
    float u_end_y;
    float u_width;
    float u_height;
    float u_resolution_scale;
    u32 u_buffer_base_offset;
    u32 u_mask_or_bits;
    float u_depth_value;
  };
  const VRAMWriteUBOData uniforms = {static_cast<float>(x % VRAM_WIDTH),
                                     static_cast<float>(y % VRAM_HEIGHT),
                                     static_cast<float>((x + width) % VRAM_WIDTH),
                                     static_cast<float>((y + height) % VRAM_HEIGHT),
                                     static_cast<float>(width),
                                     static_cast<float>(height),
                                     static_cast<float>(m_resolution_scale),
                                     map_index,
                                     (set_mask) ? 0x8000u : 0x00,
                                     GetCurrentNormalizedVertexDepth()};

  // the viewport should already be set to the full vram, so just adjust the scissor
  const GSVector4i scaled_bounds = bounds.mul32l(GSVector4i(m_resolution_scale));
  g_gpu_device->SetScissor(scaled_bounds);

  g_gpu_device->SetPipeline(m_vram_write_pipelines[BoolToUInt8(check_mask && m_write_mask_as_depth)].get());

  if (upload_texture)
    g_gpu_device->SetTextureSampler(0, upload_texture.get(), g_gpu_device->GetNearestSampler());
  else
    g_gpu_device->SetTextureBuffer(0, m_vram_upload_buffer.get());

  DrawScreenQuad(scaled_bounds, m_vram_texture->GetSizeVec(), GSVector4::zero(), &uniforms, sizeof(uniforms));

  RestoreDeviceContext();
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask, bool check_mask)
{
  FlushRender();

  GL_SCOPE_FMT("CopyVRAM({}x{} @ {},{} => {},{}", width, height, src_x, src_y, dst_x, dst_y);

  // masking enabled, oversized, or overlapping
  const GSVector4i src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
  const GSVector4i dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
  const bool intersect_with_draw = m_vram_dirty_draw_rect.rintersects(src_bounds);
  const bool intersect_with_write = m_vram_dirty_write_rect.rintersects(src_bounds);
  const bool overlaps_with_self = src_bounds.rintersects(dst_bounds);
  const bool use_shader =
    (set_mask || check_mask || ((src_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
     ((src_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT || ((dst_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
     ((dst_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT) ||
    (!intersect_with_draw && !intersect_with_write && !overlaps_with_self);

  // If we're copying a region that hasn't been drawn to, and we're using the TC, we can do it in local memory.
  if (m_use_texture_cache && !GPUTextureCache::IsRectDrawn(src_bounds))
  {
    GL_INS("Performed in local memory.");
    GPUTextureCache::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height, set_mask, check_mask, src_bounds, dst_bounds);
    UpdateVRAMOnGPU(dst_bounds.left, dst_bounds.top, dst_bounds.width(), dst_bounds.height(),
                    &g_vram[dst_bounds.top * VRAM_WIDTH + dst_bounds.left], VRAM_WIDTH * sizeof(u16), false, false,
                    dst_bounds);
    return;
  }
  else if (ShouldDrawWithSoftwareRenderer())
  {
    GPU_SW_Rasterizer::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height, set_mask, check_mask);
  }

  if (use_shader || IsUsingMultisampling())
  {
    if (intersect_with_draw || intersect_with_write)
      UpdateVRAMReadTexture(intersect_with_draw, intersect_with_write);
    AddUnclampedDrawnRectangle(dst_bounds);

    DeactivateROV();

    struct VRAMCopyUBOData
    {
      float u_src_x;
      float u_src_y;
      float u_dst_x;
      float u_dst_y;
      float u_end_x;
      float u_end_y;
      float u_vram_width;
      float u_vram_height;
      float u_resolution_scale;
      u32 u_set_mask_bit;
      float u_depth_value;
      u32 pad;
    };
    const VRAMCopyUBOData uniforms = {static_cast<float>((src_x % VRAM_WIDTH) * m_resolution_scale),
                                      static_cast<float>((src_y % VRAM_HEIGHT) * m_resolution_scale),
                                      static_cast<float>((dst_x % VRAM_WIDTH) * m_resolution_scale),
                                      static_cast<float>((dst_y % VRAM_HEIGHT) * m_resolution_scale),
                                      static_cast<float>(((dst_x + width) % VRAM_WIDTH) * m_resolution_scale),
                                      static_cast<float>(((dst_y + height) % VRAM_HEIGHT) * m_resolution_scale),
                                      static_cast<float>(m_vram_texture->GetWidth()),
                                      static_cast<float>(m_vram_texture->GetHeight()),
                                      static_cast<float>(m_resolution_scale),
                                      BoolToUInt32(set_mask),
                                      GetCurrentNormalizedVertexDepth(),
                                      0u};

    // VRAM read texture should already be bound.
    g_gpu_device->SetPipeline(m_vram_copy_pipelines[BoolToUInt8(check_mask && m_write_mask_as_depth)].get());
    g_gpu_device->SetTextureSampler(0, m_vram_read_texture.get(), g_gpu_device->GetNearestSampler());

    const GSVector4i dst_bounds_scaled = dst_bounds.mul32l(GSVector4i(m_resolution_scale));
    g_gpu_device->SetScissor(dst_bounds_scaled);
    DrawScreenQuad(dst_bounds_scaled, m_vram_texture->GetSizeVec(), GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), &uniforms,
                   sizeof(uniforms));
    RestoreDeviceContext();

    if (check_mask && !m_pgxp_depth_buffer)
      m_current_depth++;

    return;
  }

  GPUTexture* src_tex = m_vram_texture.get();
  if (!g_gpu_device->GetFeatures().texture_copy_to_self || overlaps_with_self)
  {
    src_tex = m_vram_read_texture.get();
    if (intersect_with_draw || intersect_with_write)
      UpdateVRAMReadTexture(intersect_with_draw, intersect_with_write);
  }

  // We don't have it in local memory, so TC can't read it.
  if (intersect_with_draw || m_use_texture_cache)
  {
    AddUnclampedDrawnRectangle(dst_bounds);
  }
  else if (intersect_with_write)
  {
    AddWrittenRectangle(dst_bounds);
  }
  else
  {
    const bool use_write =
      (!m_vram_dirty_write_rect.eq(INVALID_RECT) && !m_vram_dirty_draw_rect.eq(INVALID_RECT) &&
       RectDistance(m_vram_dirty_write_rect, dst_bounds) < RectDistance(m_vram_dirty_draw_rect, dst_bounds));
    if (use_write)
      AddWrittenRectangle(dst_bounds);
    else
      AddUnclampedDrawnRectangle(dst_bounds);
  }

  if (check_mask)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }

  g_gpu_device->CopyTextureRegion(m_vram_texture.get(), dst_x * m_resolution_scale, dst_y * m_resolution_scale, 0, 0,
                                  src_tex, src_x * m_resolution_scale, src_y * m_resolution_scale, 0, 0,
                                  width * m_resolution_scale, height * m_resolution_scale);
  if (src_tex != m_vram_texture.get())
    m_vram_read_texture->MakeReadyForSampling();
}

void GPU_HW::ClearCache()
{
  FlushRender();

  // Force the check below to fail.
  m_draw_mode.bits = INVALID_DRAW_MODE_BITS;
}

void GPU_HW::PrepareDraw(const GPUBackendDrawCommand* cmd)
{
  // TODO: avoid all this for vertex loading, only do when the type of draw changes
  BatchTextureMode texture_mode = cmd->texture_enable ? m_batch.texture_mode : BatchTextureMode::Disabled;
  GPUTextureCache::SourceKey texture_cache_key = m_texture_cache_key;
  if (cmd->texture_enable)
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (((m_draw_mode.bits ^ cmd->draw_mode.bits) & GPUDrawModeReg::TEXTURE_MODE_AND_PAGE_MASK) != 0 ||
        (cmd->draw_mode.IsUsingPalette() && m_draw_mode.palette_reg.bits != cmd->palette.bits) ||
        texture_mode == BatchTextureMode::Disabled)

    {
      m_draw_mode.mode_reg.bits = cmd->draw_mode.bits;
      m_draw_mode.palette_reg.bits = cmd->palette.bits;

      // start by assuming we can use the TC
      bool use_texture_cache = m_use_texture_cache;

      // check that the palette isn't in a drawn area
      if (m_draw_mode.mode_reg.IsUsingPalette())
      {
        const GSVector4i palette_rect =
          GetPaletteRect(m_draw_mode.palette_reg, m_draw_mode.mode_reg.texture_mode, use_texture_cache);
        if (!use_texture_cache || GPUTextureCache::IsRectDrawn(palette_rect))
        {
          if (use_texture_cache)
            GL_INS_FMT("Palette at {} is in drawn area, can't use TC", palette_rect);
          use_texture_cache = false;

          const bool update_drawn = palette_rect.rintersects(m_vram_dirty_draw_rect);
          const bool update_written = palette_rect.rintersects(m_vram_dirty_write_rect);
          if (update_drawn || update_written)
          {
            GL_INS("Palette in VRAM dirty area, flushing cache");
            if (!IsFlushed())
              FlushRender();

            UpdateVRAMReadTexture(update_drawn, update_written);
          }
        }
      }

      m_compute_uv_range = (m_clamp_uvs || m_texture_dumping);

      const GPUTextureMode gpu_texture_mode =
        (m_draw_mode.mode_reg.texture_mode == GPUTextureMode::Reserved_Direct16Bit) ? GPUTextureMode::Direct16Bit :
                                                                                      m_draw_mode.mode_reg.texture_mode;
      const GSVector4i page_rect = GetTextureRect(m_draw_mode.mode_reg.texture_page, m_draw_mode.mode_reg.texture_mode);

      // TODO: This will result in incorrect global-space UVs when the texture page wraps around.
      // Need to deal with it if it becomes a problem.
      m_current_texture_page_offset[0] = static_cast<s32>(m_draw_mode.mode_reg.GetTexturePageBaseX());
      m_current_texture_page_offset[1] = static_cast<s32>(m_draw_mode.mode_reg.GetTexturePageBaseY());

      if (use_texture_cache)
      {
        texture_mode = BatchTextureMode::PageTexture;
        texture_cache_key =
          GPUTextureCache::SourceKey(m_draw_mode.mode_reg.texture_page, m_draw_mode.palette_reg, gpu_texture_mode);

        const bool is_drawn = GPUTextureCache::IsRectDrawn(page_rect);
        if (is_drawn)
          GL_INS_FMT("Texpage [{}] {} is drawn in TC, checking UV ranges", texture_cache_key.page, page_rect);

        m_texpage_dirty =
          (is_drawn ? TEXPAGE_DIRTY_PAGE_RECT : 0) | (m_texture_dumping ? TEXPAGE_DIRTY_ONLY_UV_RECT : 0);
        m_compute_uv_range |= ShouldCheckForTexPageOverlap();
      }
      else
      {
        texture_mode = static_cast<BatchTextureMode>(gpu_texture_mode);
        m_texpage_dirty = (m_vram_dirty_draw_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_DRAWN_RECT : 0) |
                          (m_vram_dirty_write_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_WRITTEN_RECT : 0);
        if (m_texpage_dirty & TEXPAGE_DIRTY_DRAWN_RECT)
          GL_INS_FMT("Texpage {} is in dirty DRAWN area {}", page_rect, m_vram_dirty_draw_rect);
        if (m_texpage_dirty & TEXPAGE_DIRTY_WRITTEN_RECT)
          GL_INS_FMT("Texpage {} is in dirty WRITTEN area {}", page_rect, m_vram_dirty_write_rect);

        // Current UV rect _must_ be cleared here, because we're only check for texpage intersection when it grows in
        // size, a switch from a non-contained page to a contained page would go undetected otherwise.
        if (m_texpage_dirty != 0)
        {
          m_compute_uv_range = true;
          m_current_uv_rect = INVALID_RECT;
        }
      }
    }
  }

  DebugAssert(
    (cmd->texture_enable &&
     ((texture_mode == BatchTextureMode::PageTexture && texture_cache_key.mode == m_draw_mode.mode_reg.texture_mode) ||
      texture_mode ==
        static_cast<BatchTextureMode>((m_draw_mode.mode_reg.texture_mode == GPUTextureMode::Reserved_Direct16Bit) ?
                                        GPUTextureMode::Direct16Bit :
                                        m_draw_mode.mode_reg.texture_mode))) ||
    (!cmd->texture_enable && texture_mode == BatchTextureMode::Disabled));
  DebugAssert(!(m_texpage_dirty & TEXPAGE_DIRTY_PAGE_RECT) || texture_mode == BatchTextureMode::PageTexture ||
              !cmd->texture_enable);

  // has any state changed which requires a new batch?
  // Reverse blending breaks with mixed transparent and opaque pixels, so we have to do one draw per polygon.
  // If we have fbfetch, we don't need to draw it in two passes. Test case: Suikoden 2 shadows.
  // TODO: make this suck less.. somehow. probably arrange the relevant bits in a comparable pattern
  const GPUTransparencyMode transparency_mode =
    cmd->transparency_enable ? cmd->draw_mode.transparency_mode : GPUTransparencyMode::Disabled;
  const bool dithering_enable = (!m_true_color && cmd->dither_enable);
  if (!IsFlushed())
  {
    if (texture_mode != m_batch.texture_mode || transparency_mode != m_batch.transparency_mode ||
        (!m_allow_shader_blend && NeedsTwoPassRendering()) || dithering_enable != m_batch.dithering ||
        m_texture_window_bits != cmd->window || m_batch.check_mask_before_draw != cmd->check_mask_before_draw ||
        m_batch.set_mask_while_drawing != cmd->set_mask_while_drawing ||
        (texture_mode == BatchTextureMode::PageTexture && m_texture_cache_key != texture_cache_key))
    {
      FlushRender();
    }
  }

  EnsureVertexBufferSpaceForCommand(cmd);

  if (m_batch_index_count == 0)
  {
    // transparency mode change
    const bool check_mask_before_draw = cmd->check_mask_before_draw;
    if (transparency_mode != GPUTransparencyMode::Disabled && !m_rov_active && !m_prefer_shader_blend &&
        !NeedsShaderBlending(transparency_mode, texture_mode, check_mask_before_draw))
    {
      static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};

      const float src_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][0];
      const float dst_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][1];
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_src_alpha_factor != src_alpha_factor ||
                            m_batch_ubo_data.u_dst_alpha_factor != dst_alpha_factor);
      m_batch_ubo_data.u_src_alpha_factor = src_alpha_factor;
      m_batch_ubo_data.u_dst_alpha_factor = dst_alpha_factor;
    }

    const bool set_mask_while_drawing = cmd->set_mask_while_drawing;
    if (m_batch.check_mask_before_draw != check_mask_before_draw ||
        m_batch.set_mask_while_drawing != set_mask_while_drawing)
    {
      m_batch.check_mask_before_draw = check_mask_before_draw;
      m_batch.set_mask_while_drawing = set_mask_while_drawing;
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_set_mask_while_drawing != BoolToUInt32(set_mask_while_drawing));
      m_batch_ubo_data.u_set_mask_while_drawing = BoolToUInt32(set_mask_while_drawing);
    }

    m_batch.interlacing = cmd->interlaced_rendering;
    if (m_batch.interlacing)
    {
      const u32 displayed_field = BoolToUInt32(cmd->active_line_lsb);
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlaced_displayed_field != displayed_field);
      m_batch_ubo_data.u_interlaced_displayed_field = displayed_field;
    }

    // update state
    m_batch.texture_mode = texture_mode;
    m_batch.transparency_mode = transparency_mode;
    m_batch.dithering = dithering_enable;
    m_texture_cache_key = texture_cache_key;

    if (m_texture_window_bits != cmd->window)
    {
      m_texture_window_bits = cmd->window;
      m_texture_window_active = (cmd->window != GPUTextureWindow{{0xFF, 0xFF, 0x00, 0x00}});
      GSVector4i::store<false>(&m_batch_ubo_data.u_texture_window[0], GSVector4i::load32(&cmd->window).u8to32());
      m_batch_ubo_dirty = true;
    }

    if (m_drawing_area_changed)
    {
      m_drawing_area_changed = false;
      SetScissor();

      if (m_pgxp_depth_buffer && m_last_depth_z < 1.0f)
      {
        FlushRender();
        CopyAndClearDepthBuffer(false);
        EnsureVertexBufferSpaceForCommand(cmd);
      }
    }
  }

  if (cmd->check_mask_before_draw)
    m_current_depth++;
}

void GPU_HW::FlushRender()
{
  const u32 base_vertex = m_batch_base_vertex;
  const u32 base_index = m_batch_base_index;
  const u32 index_count = m_batch_index_count;
  DebugAssert((m_batch_vertex_ptr != nullptr) == (m_batch_index_ptr != nullptr));
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(m_batch_vertex_count, index_count);
  if (index_count == 0)
    return;

#if defined(_DEBUG) || defined(_DEVEL)
  GL_SCOPE_FMT("Hardware Draw {}: {}", ++s_draw_number, m_current_draw_rect);
#endif

  GL_INS_FMT("Dirty draw area: {}", m_vram_dirty_draw_rect);
  if (m_compute_uv_range)
    GL_INS_FMT("UV rect: {}", m_current_uv_rect);

  const GPUTextureCache::Source* texture = nullptr;
  if (m_batch.texture_mode == BatchTextureMode::PageTexture)
  {
    texture = LookupSource(m_texture_cache_key, m_current_uv_rect,
                           m_batch.transparency_mode != GPUTransparencyMode::Disabled ?
                             GPUTextureCache::PaletteRecordFlags::HasSemiTransparentDraws :
                             GPUTextureCache::PaletteRecordFlags::None);
    if (!texture) [[unlikely]]
      m_batch.texture_mode = static_cast<BatchTextureMode>(m_texture_cache_key.mode);
  }

  if (m_batch_ubo_dirty)
  {
    g_gpu_device->UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    // m_counters.num_ubo_updates++;
    m_batch_ubo_dirty = false;
  }

  m_current_draw_rect = INVALID_RECT;
  m_current_uv_rect = INVALID_RECT;

  if (m_wireframe_mode != GPUWireframeMode::OnlyWireframe)
  {
    if (NeedsShaderBlending(m_batch.transparency_mode, m_batch.texture_mode, m_batch.check_mask_before_draw) ||
        m_rov_active || (m_use_rov_for_shader_blend && m_pgxp_depth_buffer))
    {
      DrawBatchVertices(BatchRenderMode::ShaderBlend, index_count, base_index, base_vertex, texture);
    }
    else if (NeedsTwoPassRendering())
    {
      DrawBatchVertices(BatchRenderMode::OnlyOpaque, index_count, base_index, base_vertex, texture);
      DrawBatchVertices(BatchRenderMode::OnlyTransparent, index_count, base_index, base_vertex, texture);
    }
    else
    {
      DrawBatchVertices(m_batch.GetRenderMode(), index_count, base_index, base_vertex, texture);
    }
  }

  if (m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    // This'll be less than ideal, but wireframe is for debugging, so take the perf hit.
    DeactivateROV();
    g_gpu_device->SetPipeline(m_wireframe_pipeline.get());
    g_gpu_device->DrawIndexed(index_count, base_index, base_vertex);
  }
}

void GPU_HW::DrawingAreaChanged()
{
  FlushRender();
  m_drawing_area_changed = true;
}

void GPU_HW::UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd)
{
  FlushRender();
  DeactivateROV();

  GL_SCOPE("UpdateDisplay()");

  GPUTextureCache::Compact();

  // If this is a 480i single buffer game, then rendering should complete within one vblank.
  // Therefore we should clear the depth buffer, because the drawing area may not change.
  if (m_pgxp_depth_buffer && cmd->interleaved_480i_mode)
    CopyAndClearDepthBuffer(true);

  if (g_gpu_settings.gpu_show_vram)
  {
    if (IsUsingMultisampling())
    {
      UpdateVRAMReadTexture(!m_vram_dirty_draw_rect.eq(INVALID_RECT), !m_vram_dirty_write_rect.eq(INVALID_RECT));
      m_presenter.SetDisplayTexture(m_vram_read_texture.get(), 0, 0, m_vram_read_texture->GetWidth(),
                                    m_vram_read_texture->GetHeight());
    }
    else
    {
      m_presenter.SetDisplayTexture(m_vram_texture.get(), 0, 0, m_vram_texture->GetWidth(),
                                    m_vram_texture->GetHeight());
    }

    return;
  }

  const bool interlaced = cmd->interlaced_display_enabled;
  const u32 interlaced_field = BoolToUInt32(cmd->interlaced_display_field);
  const u32 line_skip = BoolToUInt32(cmd->interlaced_display_interleaved);
  const u32 resolution_scale = cmd->display_24bit ? 1 : m_resolution_scale;
  const u32 scaled_vram_offset_x = cmd->display_vram_left * resolution_scale;
  const u32 scaled_vram_offset_y =
    cmd->display_vram_top * resolution_scale +
    (BoolToUInt8(cmd->interlaced_display_field) & BoolToUInt8(cmd->interlaced_display_interleaved));
  const u32 scaled_display_width = cmd->display_vram_width * resolution_scale;
  const u32 scaled_display_height = cmd->display_vram_height * resolution_scale;
  bool drew_anything = false;

  if (cmd->display_disabled)
  {
    m_presenter.ClearDisplayTexture();
    return;
  }
  else if (!cmd->display_24bit && line_skip == 0 && !IsUsingMultisampling() &&
           (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture->GetWidth() &&
           (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture->GetHeight() &&
           (!m_internal_postfx || !m_internal_postfx->IsActive()))
  {
    m_presenter.SetDisplayTexture(m_vram_texture.get(), scaled_vram_offset_x, scaled_vram_offset_y,
                                  scaled_display_width, scaled_display_height);

    // Fast path if no copies are needed.
    if (interlaced)
    {
      GL_INS("Deinterlace fast path");
      drew_anything = true;
      m_presenter.Deinterlace(interlaced_field);
    }
    else
    {
      GL_INS("Direct display");
    }
  }
  else
  {
    if (!g_gpu_device->ResizeTexture(&m_vram_extract_texture, scaled_display_width, scaled_display_height,
                                     GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8,
                                     GPUTexture::Flags::None)) [[unlikely]]
    {
      m_presenter.ClearDisplayTexture();
      return;
    }

    m_vram_texture->MakeReadyForSampling();
    g_gpu_device->InvalidateRenderTarget(m_vram_extract_texture.get());

    // Don't bother grabbing depth if postfx doesn't need it.
    GPUTexture* depth_source =
      (!cmd->display_24bit && m_pgxp_depth_buffer && m_internal_postfx && m_internal_postfx->NeedsDepthBuffer()) ?
        (m_depth_was_copied ? m_vram_depth_copy_texture.get() : m_vram_depth_texture.get()) :
        nullptr;
    if (depth_source &&
        g_gpu_device->ResizeTexture(&m_vram_extract_depth_texture, scaled_display_width, scaled_display_height,
                                    GPUTexture::Type::RenderTarget, VRAM_DS_COLOR_FORMAT, GPUTexture::Flags::None))
    {
      depth_source->MakeReadyForSampling();
      g_gpu_device->InvalidateRenderTarget(m_vram_extract_depth_texture.get());

      GPUTexture* targets[] = {m_vram_extract_texture.get(), m_vram_extract_depth_texture.get()};
      g_gpu_device->SetRenderTargets(targets, static_cast<u32>(std::size(targets)), nullptr);
      g_gpu_device->SetPipeline(m_vram_extract_pipeline[2].get());

      g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, depth_source, g_gpu_device->GetNearestSampler());
    }
    else
    {
      g_gpu_device->SetRenderTarget(m_vram_extract_texture.get());
      g_gpu_device->SetPipeline(m_vram_extract_pipeline[BoolToUInt8(cmd->display_24bit)].get());
      g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
    }

    g_gpu_device->SetViewportAndScissor(0, 0, scaled_display_width, scaled_display_height);

    const u32 reinterpret_start_x = cmd->X * resolution_scale;
    const u32 skip_x = (cmd->display_vram_left - cmd->X) * resolution_scale;
    GL_INS_FMT("VRAM extract, depth = {}, 24bpp = {}, skip_x = {}, line_skip = {}", depth_source ? "yes" : "no",
               cmd->display_24bit, skip_x, line_skip);
    GL_INS_FMT("Source: {},{} => {},{} ({}x{})", reinterpret_start_x, scaled_vram_offset_y,
               reinterpret_start_x + scaled_display_width, (scaled_vram_offset_y + scaled_display_height) << line_skip,
               scaled_display_width, scaled_display_height);

    struct ExtractUniforms
    {
      u32 vram_offset_x;
      u32 vram_offset_y;
      float skip_x;
      float line_skip;
    };
    const ExtractUniforms uniforms = {reinterpret_start_x, scaled_vram_offset_y, static_cast<float>(skip_x),
                                      static_cast<float>(line_skip ? 2 : 1)};
    g_gpu_device->DrawWithPushConstants(3, 0, &uniforms, sizeof(uniforms));

    m_vram_extract_texture->MakeReadyForSampling();
    if (depth_source)
    {
      // Thanks DX11...
      m_vram_extract_depth_texture->MakeReadyForSampling();
      g_gpu_device->SetTextureSampler(1, nullptr, nullptr);
    }

    drew_anything = true;

    m_presenter.SetDisplayTexture(m_vram_extract_texture.get(), 0, 0, scaled_display_width, scaled_display_height);

    // Apply internal postfx if enabled.
    if (m_internal_postfx && m_internal_postfx->IsActive() &&
        m_internal_postfx->CheckTargets(scaled_display_width, scaled_display_height, m_vram_texture->GetFormat(),
                                        scaled_display_width, scaled_display_height, scaled_display_width,
                                        scaled_display_height))
    {
      GPUTexture* const postfx_output = m_internal_postfx->GetOutputTexture();
      m_internal_postfx->Apply(
        m_vram_extract_texture.get(), depth_source ? m_vram_extract_depth_texture.get() : nullptr,
        m_internal_postfx->GetOutputTexture(), GSVector4i(0, 0, scaled_display_width, scaled_display_height),
        m_presenter.GetDisplayWidth(), m_presenter.GetDisplayHeight(), cmd->display_vram_width,
        cmd->display_vram_height);
      m_presenter.SetDisplayTexture(postfx_output, 0, 0, postfx_output->GetWidth(), postfx_output->GetHeight());
    }

    if (g_gpu_settings.display_24bit_chroma_smoothing)
    {
      if (m_presenter.ApplyChromaSmoothing())
      {
        if (interlaced)
          m_presenter.Deinterlace(interlaced_field);
      }
    }
    else
    {
      if (interlaced)
        m_presenter.Deinterlace(interlaced_field);
    }
  }

  if (m_downsample_mode != GPUDownsampleMode::Disabled && !cmd->display_24bit)
  {
    DebugAssert(m_presenter.HasDisplayTexture());
    DownsampleFramebuffer();
  }

  if (drew_anything)
    RestoreDeviceContext();
}

void GPU_HW::UpdateDownsamplingLevels()
{
  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    m_downsample_scale_or_levels = 0;
    u32 current_width = VRAM_WIDTH * m_resolution_scale;
    while (current_width >= VRAM_WIDTH)
    {
      m_downsample_scale_or_levels++;
      current_width /= 2;
    }
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    m_downsample_scale_or_levels = m_resolution_scale / GetBoxDownsampleScale(m_resolution_scale);
  }
  else
  {
    m_downsample_scale_or_levels = 0;
  }

  // Toss downsampling buffer, it's likely going to change resolution.
  g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
}

void GPU_HW::OnBufferSwapped()
{
  GL_INS("OnBufferSwapped()");
  m_depth_was_copied = false;
}

void GPU_HW::DownsampleFramebuffer()
{
  GPUTexture* source = m_presenter.GetDisplayTexture();
  const u32 left = m_presenter.GetDisplayTextureViewX();
  const u32 top = m_presenter.GetDisplayTextureViewY();
  const u32 width = m_presenter.GetDisplayTextureViewWidth();
  const u32 height = m_presenter.GetDisplayTextureViewHeight();

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
    DownsampleFramebufferAdaptive(source, left, top, width, height);
  else
    DownsampleFramebufferBoxFilter(source, left, top, width, height);
}

void GPU_HW::DownsampleFramebufferAdaptive(GPUTexture* source, u32 left, u32 top, u32 width, u32 height)
{
  GL_SCOPE_FMT("DownsampleFramebufferAdaptive ({},{} => {},{})", left, top, left + width, left + height);

  struct SmoothingUBOData
  {
    float min_uv[2];
    float max_uv[2];
    float rcp_size[2];
    float lod;
  };

  if (!m_downsample_texture || m_downsample_texture->GetWidth() != width || m_downsample_texture->GetHeight() != height)
  {
    g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
    m_downsample_texture = g_gpu_device->FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                                      VRAM_RT_FORMAT, GPUTexture::Flags::None);
  }
  std::unique_ptr<GPUTexture, GPUDevice::PooledTextureDeleter> level_texture =
    g_gpu_device->FetchAutoRecycleTexture(width, height, 1, m_downsample_scale_or_levels, 1, GPUTexture::Type::Texture,
                                          VRAM_RT_FORMAT, GPUTexture::Flags::None);
  std::unique_ptr<GPUTexture, GPUDevice::PooledTextureDeleter> weight_texture = g_gpu_device->FetchAutoRecycleTexture(
    std::max(width >> (m_downsample_scale_or_levels - 1), 1u),
    std::max(height >> (m_downsample_scale_or_levels - 1), 1u), 1, 1, 1, GPUTexture::Type::RenderTarget,
    GPUTexture::Format::R8, GPUTexture::Flags::None);
  if (!m_downsample_texture || !level_texture || !weight_texture)
  {
    ERROR_LOG("Failed to create {}x{} RTs for adaptive downsampling", width, height);
    return;
  }

  g_gpu_device->CopyTextureRegion(level_texture.get(), 0, 0, 0, 0, source, left, top, 0, 0, width, height);
  g_gpu_device->SetTextureSampler(0, level_texture.get(), m_downsample_lod_sampler.get());

  SmoothingUBOData uniforms;

  // create mip chain
  for (u32 level = 1; level < m_downsample_scale_or_levels; level++)
  {
    GL_SCOPE_FMT("Create miplevel {}", level);

    const u32 level_width = width >> level;
    const u32 level_height = height >> level;
    const float rcp_width = 1.0f / static_cast<float>(level_texture->GetMipWidth(level));
    const float rcp_height = 1.0f / static_cast<float>(level_texture->GetMipHeight(level));
    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = static_cast<float>(level_width) * rcp_width;
    uniforms.max_uv[1] = static_cast<float>(level_height) * rcp_height;
    uniforms.rcp_size[0] = rcp_width * 0.25f;
    uniforms.rcp_size[1] = rcp_height * 0.25f;
    uniforms.lod = static_cast<float>(level - 1);

    g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, level_width, level_height));
    g_gpu_device->SetPipeline(m_downsample_pass_pipeline.get());
    g_gpu_device->DrawWithPushConstants(3, 0, &uniforms, sizeof(uniforms));
    g_gpu_device->CopyTextureRegion(level_texture.get(), 0, 0, 0, level, m_downsample_texture.get(), 0, 0, 0, 0,
                                    level_width, level_height);
  }

  // blur pass at lowest level
  {
    GL_SCOPE("Blur");

    const u32 last_level = m_downsample_scale_or_levels - 1;
    const u32 last_width = level_texture->GetMipWidth(last_level);
    const u32 last_height = level_texture->GetMipHeight(last_level);
    const float rcp_width = 1.0f / static_cast<float>(m_downsample_texture->GetWidth());
    const float rcp_height = 1.0f / static_cast<float>(m_downsample_texture->GetHeight());
    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = static_cast<float>(last_width) * rcp_width;
    uniforms.max_uv[1] = static_cast<float>(last_height) * rcp_height;
    uniforms.rcp_size[0] = rcp_width;
    uniforms.rcp_size[1] = rcp_height;
    uniforms.lod = 0.0f;

    m_downsample_texture->MakeReadyForSampling();
    g_gpu_device->InvalidateRenderTarget(weight_texture.get());
    g_gpu_device->SetRenderTarget(weight_texture.get());
    g_gpu_device->SetTextureSampler(0, m_downsample_texture.get(), g_gpu_device->GetNearestSampler());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, last_width, last_height));
    g_gpu_device->SetPipeline(m_downsample_blur_pipeline.get());
    g_gpu_device->DrawWithPushConstants(3, 0, &uniforms, sizeof(uniforms));
    weight_texture->MakeReadyForSampling();
  }

  // composite downsampled and upsampled images together
  {
    GL_SCOPE("Composite");

    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = 1.0f;
    uniforms.max_uv[1] = 1.0f;
    uniforms.lod = static_cast<float>(level_texture->GetLevels() - 1);

    g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetTextureSampler(0, level_texture.get(), m_downsample_composite_sampler.get());
    g_gpu_device->SetTextureSampler(1, weight_texture.get(), m_downsample_lod_sampler.get());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, width, height));
    g_gpu_device->SetPipeline(m_downsample_composite_pipeline.get());
    g_gpu_device->DrawWithPushConstants(3, 0, &uniforms, sizeof(uniforms));
    m_downsample_texture->MakeReadyForSampling();
  }

  RestoreDeviceContext();

  m_presenter.SetDisplayTexture(m_downsample_texture.get(), 0, 0, width, height);
}

void GPU_HW::DownsampleFramebufferBoxFilter(GPUTexture* source, u32 left, u32 top, u32 width, u32 height)
{
  GL_SCOPE_FMT("DownsampleFramebufferBoxFilter({},{} => {},{} ({}x{})", left, top, left + width, top + height, width,
               height);

  const u32 ds_width = width / m_downsample_scale_or_levels;
  const u32 ds_height = height / m_downsample_scale_or_levels;

  if (!g_gpu_device->ResizeTexture(&m_downsample_texture, ds_width, ds_height, GPUTexture::Type::RenderTarget,
                                   VRAM_RT_FORMAT, GPUTexture::Flags::None, false))
  {
    ERROR_LOG("Failed to create {}x{} RT for box downsampling", width, height);
    return;
  }

  source->MakeReadyForSampling();

  const u32 uniforms[4] = {left, top, 0u, 0u};

  g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
  g_gpu_device->SetRenderTarget(m_downsample_texture.get());
  g_gpu_device->SetPipeline(m_downsample_pass_pipeline.get());
  g_gpu_device->SetTextureSampler(0, source, g_gpu_device->GetNearestSampler());
  g_gpu_device->SetViewportAndScissor(0, 0, ds_width, ds_height);
  g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

  RestoreDeviceContext();

  m_presenter.SetDisplayTexture(m_downsample_texture.get(), 0, 0, ds_width, ds_height);
}

void GPU_HW::LoadInternalPostProcessing()
{
  static constexpr const char* section = PostProcessing::Config::INTERNAL_CHAIN_SECTION;

  auto lock = Host::GetSettingsLock();
  const SettingsInterface& si = GPUPresenter::GetPostProcessingSettingsInterface(section);

  if (PostProcessing::Config::GetStageCount(si, section) == 0 || !PostProcessing::Config::IsEnabled(si, section))
    return;

  m_internal_postfx = std::make_unique<PostProcessing::Chain>(section);
  m_internal_postfx->LoadStages(lock, si, false);
}

void GPU_HW::UpdatePostProcessingSettings(bool force_reload)
{
  static constexpr const char* section = PostProcessing::Config::INTERNAL_CHAIN_SECTION;

  auto lock = Host::GetSettingsLock();
  const SettingsInterface& si = *Host::GetSettingsInterface();

  // Don't delete the chain if we're just temporarily disabling.
  if (PostProcessing::Config::GetStageCount(si, section) == 0)
  {
    m_internal_postfx.reset();
  }
  else
  {
    if (!m_internal_postfx || force_reload)
    {
      if (!m_internal_postfx)
        m_internal_postfx = std::make_unique<PostProcessing::Chain>(section);
      m_internal_postfx->LoadStages(lock, si, true);
    }
    else
    {
      m_internal_postfx->UpdateSettings(lock, si);
    }
  }
}

std::unique_ptr<GPUBackend> GPUBackend::CreateHardwareBackend(GPUPresenter& presenter)
{
  return std::make_unique<GPU_HW>(presenter);
}
