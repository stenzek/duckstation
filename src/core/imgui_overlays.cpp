// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_overlays.h"
#include "achievements.h"
#include "cdrom.h"
#include "controller.h"
#include "cpu_core_private.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_thread.h"
#include "gte.h"
#include "host.h"
#include "mdec.h"
#include "performance_counters.h"
#include "settings.h"
#include "spu.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_animated.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"
#include "fmt/chrono.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <span>

LOG_CHANNEL(ImGuiManager);

namespace ImGuiManager {

static constexpr float FIXED_BOLD_WEIGHT = 700.0f;

namespace {

struct InputOverlayState
{
  static constexpr u32 MAX_BINDS = 32;

  struct PadState
  {
    ControllerType ctype;
    u8 port;
    u8 slot;
    bool multitap;
    u32 icon_color;
    float vibration_state[InputManager::MAX_MOTORS_PER_PAD];
    float bind_state[MAX_BINDS];
  };

  std::array<PadState, NUM_CONTROLLER_AND_CARD_PORTS> pads;
  u32 num_active_pads = 0;
};

struct InputOverlayStateUpdateBuffer
{
  u32 num_active_pads = 0;
  InputOverlayState::PadState pads[0];
};

#ifndef __ANDROID__

struct DebugWindowInfo
{
  const char* name;
  const char* window_title;
  const char* icon_name;
  void (*draw_func)(float);
  u32 default_width;
  u32 default_height;
};

#endif

} // namespace

static void FormatProcessorStat(SmallStringBase& text, double usage, double time);
static void SetStatusIndicatorIcons(SmallStringBase& text, bool paused);
static void DrawPerformanceOverlay(const GPUBackend* gpu, float& position_y, float scale, float margin, float spacing);
static void DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawEnhancementsOverlay(const GPUBackend* gpu);
static void DrawInputsOverlay();
static void UpdateInputOverlay(void* buffer);

#ifndef __ANDROID__

static constexpr size_t NUM_DEBUG_WINDOWS = 7;
static constexpr const char* DEBUG_WINDOW_CONFIG_SECTION = "DebugWindows";
static constexpr const std::array<DebugWindowInfo, NUM_DEBUG_WINDOWS> s_debug_window_info = {{
  {"Freecam", "Free Camera", ":icons/applications-system.png", &GTE::DrawFreecamWindow, 500, 425},
  {"SPU", "SPU State", ":icons/applications-system.png", &SPU::DrawDebugStateWindow, 800, 915},
  {"CDROM", "CD-ROM State", ":icons/applications-system.png", &CDROM::DrawDebugWindow, 800, 540},
  {"GPU", "GPU State", ":icons/applications-system.png", [](float sc) { g_gpu.DrawDebugStateWindow(sc); }, 450, 550},
  {"DMA", "DMA State", ":icons/applications-system.png", &DMA::DrawDebugStateWindow, 860, 180},
  {"MDEC", "MDEC State", ":icons/applications-system.png", &MDEC::DrawDebugStateWindow, 300, 350},
  {"Timers", "Timers State", ":icons/applications-system.png", &Timers::DrawDebugStateWindow, 800, 95},
}};
static std::array<ImGuiManager::AuxiliaryRenderWindowState, NUM_DEBUG_WINDOWS> s_debug_window_state = {};

#endif

static InputOverlayState s_input_overlay_state = {};

} // namespace ImGuiManager

static std::tuple<float, float> GetMinMax(std::span<const float> values)
{
  GSVector4 vmin(GSVector4::load<false>(values.data()));
  GSVector4 vmax(vmin);

  const u32 count = static_cast<u32>(values.size());
  const u32 aligned_count = Common::AlignDownPow2(count, 4);
  u32 i = 4;
  for (; i < aligned_count; i += 4)
  {
    const GSVector4 v(GSVector4::load<false>(&values[i]));
    vmin = vmin.min(v);
    vmax = vmax.max(v);
  }

  float min = std::min(vmin.x, std::min(vmin.y, std::min(vmin.z, vmin.w)));
  float max = std::max(vmax.x, std::max(vmax.y, std::max(vmax.z, vmax.w)));
  for (; i < count; i++)
  {
    min = std::min(min, values[i]);
    max = std::max(max, values[i]);
  }

  return std::tie(min, max);
}

bool ImGuiManager::AreAnyDebugWindowsEnabled(const SettingsInterface& si)
{
#ifndef __ANDROID__
  const bool block_all = Achievements::IsHardcoreModeActive();
  if (block_all)
    return false;

  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    const DebugWindowInfo& info = s_debug_window_info[i];
    if (si.GetBoolValue(DEBUG_WINDOW_CONFIG_SECTION, info.name, false))
      return true;
  }
#endif

  return false;
}

bool ImGuiManager::IsSPUDebugWindowEnabled()
{
#ifndef __ANDROID__
  return (s_debug_window_state[1].window_handle != nullptr);
#else
  return false;
#endif
}

bool ImGuiManager::UpdateDebugWindowConfig()
{
#ifndef __ANDROID__
  const bool block_all = Achievements::IsHardcoreModeActive();
  bool was_changed = false;

  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    const DebugWindowInfo& info = s_debug_window_info[i];

    const bool current = (state.window_handle != nullptr);
    const bool enabled = (!block_all && Host::GetBaseBoolSettingValue(DEBUG_WINDOW_CONFIG_SECTION, info.name, false));
    if (enabled == current)
      continue;

    if (!enabled)
    {
      DestroyAuxiliaryRenderWindow(&state, DEBUG_WINDOW_CONFIG_SECTION, info.name);
    }
    else
    {
      Error error;
      if (!CreateAuxiliaryRenderWindow(&state, info.window_title, info.icon_name, DEBUG_WINDOW_CONFIG_SECTION,
                                       info.name, info.default_width, info.default_height, &error))
      {
        ERROR_LOG("Failed to create aux render window for {}: {}", info.name, error.GetDescription());
      }
      else
      {
        was_changed = true;
      }
    }
  }

  return was_changed;
#else
  return false;
#endif
}

void ImGuiManager::RenderDebugWindows()
{
#ifndef __ANDROID__
  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    if (!state.window_handle)
      continue;

    if (!RenderAuxiliaryRenderWindow(&state, s_debug_window_info[i].draw_func))
    {
      // window was closed, destroy it and update the configuration
      const DebugWindowInfo& info = s_debug_window_info[i];
      DestroyAuxiliaryRenderWindow(&state, DEBUG_WINDOW_CONFIG_SECTION, info.name);
      Host::SetBaseBoolSettingValue(DEBUG_WINDOW_CONFIG_SECTION, info.name, false);
      Host::CommitBaseSettingChanges();
    }
  }
#endif
}

void ImGuiManager::DestroyAllDebugWindows()
{
#ifndef __ANDROID__
  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    if (!state.window_handle)
      continue;

    ImGuiManager::DestroyAuxiliaryRenderWindow(&state, DEBUG_WINDOW_CONFIG_SECTION, s_debug_window_info[i].name);
  }
#endif
}

void ImGuiManager::RenderTextOverlays(const GPUBackend* gpu)
{
  // Don't draw anything with loading screen open, it'll be nonsensical.
  if (ImGuiFullscreen::IsLoadingScreenOpen())
    return;

  const bool paused = GPUThread::IsSystemPaused();

  const float scale = ImGuiManager::GetGlobalScale();
  const float f_margin = ImGuiManager::GetScreenMargin() * scale;
  const float margin = ImCeil(ImGuiManager::GetScreenMargin() * scale);
  const float spacing = ImCeil(5.0f * scale);
  float position_y = ImFloor(f_margin);
  DrawPerformanceOverlay(gpu, position_y, scale, margin, spacing);
  DrawMediaCaptureOverlay(position_y, scale, margin, spacing);

  if (g_gpu_settings.display_show_enhancements && !paused)
    DrawEnhancementsOverlay(gpu);

  if (g_gpu_settings.display_show_inputs && !paused)
    DrawInputsOverlay();
}

void ImGuiManager::FormatProcessorStat(SmallStringBase& text, double usage, double time)
{
  // Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
  // which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
  // so that people don't get confused, and remove the decimal places when it's there while we're at it.
  if (usage >= 99.95)
    text.append_format("100% ({:.2f}ms)", time);
  else
    text.append_format("{:.1f}% ({:.2f}ms)", usage, time);
}

void ImGuiManager::SetStatusIndicatorIcons(SmallStringBase& text, bool paused)
{
  text.clear();
  if (GTE::IsFreecamEnabled())
    text.append(ICON_EMOJI_MAGNIFIYING_GLASS_TILTED_LEFT " ");

  if (paused)
  {
    text.append(ICON_EMOJI_PAUSE);
  }
  else
  {
    const bool rewinding = System::IsRewinding();
    if (rewinding || System::IsFastForwardEnabled() || System::IsTurboEnabled())
      text.append(rewinding ? ICON_EMOJI_FAST_REVERSE : ICON_EMOJI_FAST_FORWARD);
  }

  if (!text.empty() && text.back() == ' ')
    text.pop_back();
}

static void DrawPerformanceStat(ImDrawList* dl, float& position_y, ImFont* font, float size, float alt_weight,
                                ImU32 alt_color, float shadow_offset, float rbounds, std::string_view text)
{
  static constexpr auto find_control_char = [](const std::string_view& sv, std::string_view::size_type pos) {
    const size_t len = sv.length();
    for (; pos < len; pos++)
    {
      if (sv[pos] > 0 && sv[pos] <= '\x04')
        break;
    }
    return pos;
  };

  if (text.empty())
    return;

  constexpr ImU32 color = IM_COL32(255, 255, 255, 255);
  constexpr float default_weight = 0.0f;
  std::string_view::size_type pos = 0;
  float current_weight = default_weight;
  float width = 0.0f;
  float height = 0.0f;
  do
  {
    if (text[pos] >= '\x01' && text[pos] <= '\x04')
    {
      current_weight = (text[pos] == '\x02') ? alt_weight : ((text[pos] == '\x01') ? default_weight : current_weight);
      pos++;
    }

    std::string_view::size_type epos = find_control_char(text, pos);
    const char* start_ptr = text.data() + pos;
    const char* end_ptr = text.data() + epos;
    if (start_ptr != end_ptr)
    {
      const ImVec2 text_size = font->CalcTextSizeA(size, current_weight, FLT_MAX, 0.0f, start_ptr, end_ptr);
      width += text_size.x;
      height = std::max(height, text_size.y);
    }

    pos = epos;
  } while (pos < text.length());

  ImVec2 position = ImVec2(rbounds - width, position_y);
  ImU32 current_color = color;
  pos = 0;
  current_weight = default_weight;
  do
  {
    const char ch = text[pos];
    if (ch >= '\x01' && ch <= '\x02')
    {
      current_weight = (ch == '\x02') ? alt_weight : default_weight;
      pos++;
    }
    else if (ch >= '\x03' && ch <= '\x04')
    {
      current_color = (ch == '\x04') ? alt_color : color;
      pos++;
    }

    std::string_view::size_type epos = find_control_char(text, pos);
    const char* start_ptr = text.data() + pos;
    const char* end_ptr = text.data() + ((epos == std::string_view::npos) ? text.length() : epos);
    if (start_ptr != end_ptr)
    {
      dl->AddText(font, size, current_weight, ImVec2(position.x + shadow_offset, position.y + shadow_offset),
                  IM_COL32(0, 0, 0, 100), start_ptr, end_ptr);
      dl->AddText(font, size, current_weight, position, current_color, start_ptr, end_ptr);
      position.x += font->CalcTextSizeA(size, current_weight, FLT_MAX, 0.0f, start_ptr, end_ptr).x;
    }

    pos = epos;
  } while (pos < text.length());

  position_y += height;
}

void ImGuiManager::DrawPerformanceOverlay(const GPUBackend* gpu, float& position_y, float scale, float margin,
                                          float spacing)
{
  if (!(g_gpu_settings.display_show_fps || g_gpu_settings.display_show_speed || g_gpu_settings.display_show_gpu_stats ||
        g_gpu_settings.display_show_resolution || g_gpu_settings.display_show_latency_stats ||
        g_gpu_settings.display_show_cpu_usage || g_gpu_settings.display_show_gpu_usage ||
        g_gpu_settings.display_show_frame_times ||
        (g_gpu_settings.display_show_status_indicators &&
         (GPUThread::IsSystemPaused() || System::IsFastForwardEnabled() || System::IsTurboEnabled()))))
  {
    return;
  }

  const float shadow_offset = std::ceil(1.0f * scale);
  const float status_size = std::ceil(40.0f * scale);
  ImFont* const fixed_font = ImGuiManager::GetFixedFont();
  const float fixed_font_size = ImGuiManager::GetFixedFontSize();
  ImFont* ui_font = ImGuiManager::GetTextFont();
  const float rbound = ImGui::GetIO().DisplaySize.x - margin;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  SmallString text;

  if (!GPUThread::IsSystemPaused())
  {
    const float speed = PerformanceCounters::GetEmulationSpeed();
    if (g_gpu_settings.display_show_fps)
      text.append_format("\x02G: \x04{:.2f}\x03\x02 | V: \x01\x04{:.2f}\x03", PerformanceCounters::GetFPS(),
                         PerformanceCounters::GetVPS());
    if (g_gpu_settings.display_show_speed)
    {
      text.append_format("\x02{}\x04{}% \x01\x03", text.empty() ? "" : " | ", static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.append("(Max)");
      else
        text.append_format("({:.0f}%)", target_speed * 100.0f);
    }
    if (!text.empty())
    {
      ImU32 alt_color;
      if (speed < 95.0f)
        alt_color = IM_COL32(255, 100, 100, 255);
      else if (speed > 105.0f)
        alt_color = IM_COL32(100, 255, 100, 255);
      else
        alt_color = IM_COL32(255, 255, 255, 255);

      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, alt_color, shadow_offset,
                          rbound, text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_gpu_stats)
    {
      gpu->GetStatsString(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;

      gpu->GetMemoryStatsString(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_resolution)
    {
      const u32 resolution_scale = gpu->GetResolutionScale();
      const auto [display_width, display_height] = g_gpu.GetFullDisplayResolution(); // NOTE: Racey read.
      const bool interlaced = g_gpu.IsInterlacedDisplayEnabled();
      const bool pal = g_gpu.IsInPALMode();
      text.format("{}x{} {} {} [{}x]", display_width * resolution_scale, display_height * resolution_scale,
                  pal ? "PAL" : "NTSC", interlaced ? "Interlaced" : "Progressive", resolution_scale);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_latency_stats)
    {
      System::FormatLatencyStats(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_cpu_usage)
    {
      text.format("{:.2f}ms | {:.2f}ms | {:.2f}ms", PerformanceCounters::GetMinimumFrameTime(),
                  PerformanceCounters::GetAverageFrameTime(), PerformanceCounters::GetMaximumFrameTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;

      if (g_settings.cpu_overclock_active || CPU::g_state.using_interpreter ||
          g_settings.cpu_execution_mode != CPUExecutionMode::Recompiler || g_settings.cpu_recompiler_icache ||
          g_settings.cpu_recompiler_memory_exceptions)
      {
        bool first = true;
        text.assign("\x02"
                    "CPU[");
        if (g_settings.cpu_overclock_active)
        {
          text.append_format("{}", g_settings.GetCPUOverclockPercent());
          first = false;
        }
        if (CPU::g_state.using_interpreter)
        {
          text.append_format("{}{}", first ? "" : "/", "I");
        }
        else if (g_settings.cpu_execution_mode == CPUExecutionMode::CachedInterpreter)
        {
          text.append_format("{}{}", first ? "" : "/", "CI");
        }
        else
        {
          if (g_settings.cpu_recompiler_icache)
          {
            text.append_format("{}{}", first ? "" : "/", "IC");
            first = false;
          }
          if (g_settings.cpu_recompiler_memory_exceptions)
            text.append_format("{}{}", first ? "" : "/", "ME");
        }

        text.append("]: \x01");
      }
      else
      {
        text.assign("\x02"
                    "CPU: \x01");
      }
      FormatProcessorStat(text, PerformanceCounters::GetCPUThreadUsage(),
                          PerformanceCounters::GetCPUThreadAverageTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;

      if (g_gpu_settings.gpu_use_thread)
      {
        text.assign("\x02RNDR: \x01");
        FormatProcessorStat(text, PerformanceCounters::GetGPUThreadUsage(),
                            PerformanceCounters::GetGPUThreadAverageTime());
        DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                            text);
        position_y += spacing;
      }

#ifndef __ANDROID__
      if (MediaCapture* cap = System::GetMediaCapture())
      {
        text.assign("\x02"
                    "CAP: \x01");
        FormatProcessorStat(text, cap->GetCaptureThreadUsage(), cap->GetCaptureThreadTime());
        DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                            text);
        position_y += spacing;
      }
#endif
    }

    if (g_gpu_settings.display_show_gpu_usage && g_gpu_device->IsGPUTimingEnabled())
    {
      text.assign("\x02GPU: \x01");
      FormatProcessorStat(text, PerformanceCounters::GetGPUUsage(), PerformanceCounters::GetGPUAverageTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, shadow_offset, rbound,
                          text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_frame_times)
      DrawFrameTimeOverlay(position_y, scale, margin, spacing);

    if (g_gpu_settings.display_show_status_indicators)
    {
      SetStatusIndicatorIcons(text, false);
      DrawPerformanceStat(dl, position_y, ui_font, status_size, 0.0f, 0, shadow_offset, rbound, text);
    }
  }
  else if (g_gpu_settings.display_show_status_indicators && !FullscreenUI::HasActiveWindow())
  {
    SetStatusIndicatorIcons(text, true);
    DrawPerformanceStat(dl, position_y, ui_font, status_size, 0.0f, 0, shadow_offset, rbound, text);
  }
}

void ImGuiManager::DrawEnhancementsOverlay(const GPUBackend* gpu)
{
  LargeString text;
  text.append_format("{} {}-{}", Settings::GetConsoleRegionName(System::GetRegion()),
                     GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()),
                     GPUBackend::IsUsingHardwareBackend() ? "HW" : "SW");

  if (g_settings.rewind_enable)
    text.append_format(" RW={}/{}", g_settings.rewind_save_frequency, g_settings.rewind_save_slots);
  if (g_settings.IsRunaheadEnabled())
    text.append_format(" RA={}", g_settings.runahead_frames);

  if (g_settings.cpu_overclock_active)
    text.append_format(" CPU={}%", g_settings.GetCPUOverclockPercent());
  if (g_settings.enable_8mb_ram)
    text.append(" 8MB");
  if (g_settings.cdrom_read_speedup != 1)
    text.append_format(" CDR={}x", g_settings.cdrom_read_speedup);
  if (g_settings.cdrom_seek_speedup != 1)
    text.append_format(" CDS={}x", g_settings.cdrom_seek_speedup);
  if (g_gpu_settings.gpu_resolution_scale != 1)
    text.append_format(" IR={}x", g_gpu_settings.gpu_resolution_scale);
  if (g_gpu_settings.gpu_multisamples != 1)
  {
    text.append_format(" {}x{}", g_gpu_settings.gpu_multisamples,
                       g_gpu_settings.gpu_per_sample_shading ? "SSAA" : "MSAA");
  }
  if (g_gpu_settings.gpu_dithering_mode != GPUDitheringMode::Unscaled)
    text.append_format(" DT={}", Settings::GetGPUDitheringModeName(g_gpu_settings.gpu_dithering_mode));
  text.append_format(" DI={}", Settings::GetDisplayDeinterlacingModeName(g_gpu_settings.display_deinterlacing_mode));
  if (g_settings.gpu_force_video_timing == ForceVideoTimingMode::NTSC && System::GetRegion() == ConsoleRegion::PAL)
    text.append(" PAL60");
  if (g_settings.gpu_force_video_timing == ForceVideoTimingMode::PAL && System::GetRegion() != ConsoleRegion::PAL)
    text.append(" NTSC50");
  if (g_gpu_settings.gpu_texture_filter != GPUTextureFilter::Nearest)
  {
    if (g_gpu_settings.gpu_sprite_texture_filter != g_gpu_settings.gpu_texture_filter)
    {
      text.append_format(" {}/{}", Settings::GetTextureFilterName(g_gpu_settings.gpu_texture_filter),
                         Settings::GetTextureFilterName(g_gpu_settings.gpu_sprite_texture_filter));
    }
    else
    {
      text.append_format(" {}", Settings::GetTextureFilterName(g_gpu_settings.gpu_texture_filter));
    }
  }
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto &&
      g_settings.display_aspect_ratio != DisplayAspectRatio::R4_3)
  {
    text.append(" WSHack");
  }
  if (g_gpu_settings.gpu_line_detect_mode != GPULineDetectMode::Disabled)
    text.append_format(" LD={}", Settings::GetLineDetectModeName(g_gpu_settings.gpu_line_detect_mode));
  if (g_settings.gpu_pgxp_enable)
  {
    text.append(" PGXP");
    if (g_settings.gpu_pgxp_culling)
      text.append("/Cull");
    if (g_settings.gpu_pgxp_texture_correction)
      text.append("/Tex");
    if (g_settings.gpu_pgxp_color_correction)
      text.append("/Col");
    if (g_settings.gpu_pgxp_vertex_cache)
      text.append("/VC");
    if (g_settings.gpu_pgxp_cpu)
      text.append("/CPU");
    if (g_settings.gpu_pgxp_depth_buffer)
      text.append("/Depth");
  }

  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = ImGuiManager::GetScreenMargin() * scale;
  ImFont* const font = ImGuiManager::GetFixedFont();
  const float font_size = ImGuiManager::GetFixedFontSize();
  const float font_weight = 0.0f;
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - font_size;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImVec2 text_size = font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), -1.0f, text.c_str(),
                                         text.end_ptr(), nullptr);
  dl->AddText(font, font_size, font_weight,
              ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),
              IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
  dl->AddText(font, font_size, font_weight, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
              IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
}

void ImGuiManager::DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing)
{
#ifndef __ANDROID__
  MediaCapture* const cap = System::GetMediaCapture();
  if (!cap || FullscreenUI::HasActiveWindow())
    return;

  const float shadow_offset = std::ceil(scale);
  ImFont* const ui_font = ImGuiManager::GetTextFont();
  const float ui_font_size = ImGuiManager::GetOSDFontSize();
  const float ui_font_weight = 0.0f;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  static constexpr const char* ICON = ICON_PF_CIRCLE;
  const time_t elapsed_time = cap->GetElapsedTime();
  const TinyString text_msg = TinyString::from_format(" {:02d}:{:02d}:{:02d}", elapsed_time / 3600,
                                                      (elapsed_time % 3600) / 60, (elapsed_time % 3600) % 60);
  const ImVec2 icon_size = ui_font->CalcTextSizeA(ui_font_size, ui_font_weight, std::numeric_limits<float>::max(),
                                                  -1.0f, ICON, nullptr, nullptr);
  const ImVec2 text_size = ui_font->CalcTextSizeA(ui_font_size, ui_font_weight, std::numeric_limits<float>::max(),
                                                  -1.0f, text_msg.c_str(), text_msg.end_ptr(), nullptr);

  const float box_margin = 5.0f * scale;
  const ImVec2 box_size = ImVec2(ImCeil(icon_size.x + shadow_offset + text_size.x + box_margin * 2.0f),
                                 ImCeil(std::max(icon_size.x, text_size.y) + box_margin * 2.0f));
  const ImVec2 box_pos = ImVec2(ImGui::GetIO().DisplaySize.x - margin - box_size.x, position_y);
  dl->AddRectFilled(box_pos, box_pos + box_size, IM_COL32(0, 0, 0, 64), box_margin);

  const ImVec2 text_start = ImVec2(box_pos.x + box_margin, box_pos.y + box_margin);
  dl->AddText(ui_font, ui_font_size, ui_font_weight, ImVec2(text_start.x + shadow_offset, text_start.y + shadow_offset),
              IM_COL32(0, 0, 0, 100), ICON);
  dl->AddText(ui_font, ui_font_size, ui_font_weight,
              ImVec2(text_start.x + icon_size.x + shadow_offset, text_start.y + shadow_offset), IM_COL32(0, 0, 0, 100),
              text_msg.c_str(), text_msg.end_ptr());
  dl->AddText(ui_font, ui_font_size, ui_font_weight, text_start, IM_COL32(255, 0, 0, 255), ICON);
  dl->AddText(ui_font, ui_font_size, ui_font_weight, ImVec2(text_start.x + icon_size.x, text_start.y),
              IM_COL32(255, 255, 255, 255), text_msg.c_str(), text_msg.end_ptr());

  position_y += box_size.y + spacing;
#endif
}

void ImGuiManager::DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing)
{
  const float shadow_offset = std::ceil(1.0f * scale);
  ImFont* fixed_font = ImGuiManager::GetFixedFont();
  const float fixed_font_size = ImGuiManager::GetFixedFontSize();
  const float fixed_font_weight = 0.0f;

  const ImVec2 history_size(ImCeil(200.0f * scale), ImCeil(50.0f * scale));
  ImGui::SetNextWindowSize(ImVec2(history_size.x, history_size.y));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - margin - history_size.x, position_y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  if (ImGui::Begin("##frame_times", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    ImGui::PushFont(fixed_font, fixed_font_size, fixed_font_weight);

    auto [min, max] = GetMinMax(PerformanceCounters::GetFrameTimeHistory());

    // add a little bit of space either side, so we're not constantly resizing
    if ((max - min) < 4.0f)
    {
      min = min - std::fmod(min, 1.0f);
      max = max - std::fmod(max, 1.0f) + 1.0f;
      min = std::max(min - 2.0f, 0.0f);
      max += 2.0f;
    }

    ImGui::PlotEx(
      ImGuiPlotType_Lines, "##frame_times",
      [](void*, int idx) -> float {
        return PerformanceCounters::GetFrameTimeHistory()[((PerformanceCounters::GetFrameTimeHistoryPos() + idx) %
                                                           PerformanceCounters::NUM_FRAME_TIME_SAMPLES)];
      },
      nullptr, PerformanceCounters::NUM_FRAME_TIME_SAMPLES, 0, nullptr, min, max, history_size);

    ImDrawList* win_dl = ImGui::GetCurrentWindow()->DrawList;
    const ImVec2 wpos(ImGui::GetCurrentWindow()->Pos);

    TinyString text;
    text.format("{:.1f} ms", max);
    ImVec2 text_size = fixed_font->CalcTextSizeA(fixed_font_size, -1.0f, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset, wpos.y + shadow_offset),
                    IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y), IM_COL32(255, 255, 255, 255),
                    text.c_str(), text.end_ptr());

    text.format("{:.1f} ms", min);
    text_size = fixed_font->CalcTextSizeA(fixed_font_size, -1.0f, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset,
                           wpos.y + history_size.y - fixed_font_size + shadow_offset),
                    IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y + history_size.y - fixed_font_size),
                    IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(5);
  ImGui::PopStyleColor(3);

  position_y += history_size.y + spacing;
}

void ImGuiManager::UpdateInputOverlay()
{
  u32 num_active_pads = 0;
  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] != ControllerType::None)
      num_active_pads++;
  }

  const u32 buffer_size =
    sizeof(InputOverlayStateUpdateBuffer) + (static_cast<u32>(sizeof(InputOverlayState)) * num_active_pads);
  const auto& [cmd, buffer] = GPUThread::BeginASyncBufferCall(&ImGuiManager::UpdateInputOverlay, buffer_size);

  InputOverlayStateUpdateBuffer* const ubuffer = static_cast<InputOverlayStateUpdateBuffer*>(buffer);
  ubuffer->num_active_pads = num_active_pads;

  size_t out_index = 0;
  for (const u32 pad : Controller::PortDisplayOrder)
  {
    const Controller* controller = System::GetController(pad);
    if (!controller)
      continue;

    const ControllerType ctype = controller->GetType();
    const auto& [port, slot] = Controller::ConvertPadToPortAndSlot(pad);
    const bool multitap = g_settings.IsMultitapPortEnabled(port);
    InputOverlayState::PadState& pstate = ubuffer->pads[out_index++];
    pstate.port = Truncate8(port);
    pstate.slot = Truncate8(slot);
    pstate.multitap = multitap;
    pstate.ctype = ctype;
    pstate.icon_color = controller->GetInputOverlayIconColor();

    const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ctype);
    for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
    {
      const u32 bidx = bi.bind_index;

      // this will leave some uninitalized, but who cares, it won't be read on the other side
      if (bi.type >= InputBindingInfo::Type::Button && bi.type <= InputBindingInfo::Type::HalfAxis)
      {
        DebugAssert(bidx < InputOverlayState::MAX_BINDS);
        pstate.bind_state[bidx] = controller->GetBindState(bidx);
      }
      else if (bi.type == InputBindingInfo::Type::Motor)
      {
        DebugAssert(bidx < InputManager::MAX_MOTORS_PER_PAD);
        pstate.vibration_state[bidx] = controller->GetVibrationMotorState(bidx);
      }
    }
  }

  GPUThread::EndASyncBufferCall(cmd);
}

void ImGuiManager::UpdateInputOverlay(void* buffer)
{
  InputOverlayStateUpdateBuffer* const RESTRICT ubuffer = static_cast<InputOverlayStateUpdateBuffer*>(buffer);
  DebugAssert(ubuffer->num_active_pads < NUM_CONTROLLER_AND_CARD_PORTS);
  s_input_overlay_state.num_active_pads = ubuffer->num_active_pads;
  for (u32 i = 0; i < ubuffer->num_active_pads; i++)
    s_input_overlay_state.pads[i] = ubuffer->pads[i];
}

void ImGuiManager::DrawInputsOverlay()
{
  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = ImCeil(1.0f * scale);
  const float margin = ImGuiManager::GetScreenMargin() * scale;
  const float spacing = ImCeil(5.0f * scale);
  ImFont* const font = ImGuiManager::GetTextFont();
  const float font_size = ImGuiManager::GetOSDFontSize();
  const float font_weight = 400.0f;

  static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);
  static constexpr u32 shadow_color = IM_COL32(0x00, 0x00, 0x00, 100);

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  float current_x = ImFloor(margin);
  float current_y =
    ImFloor(display_size.y - margin -
            ((static_cast<float>(s_input_overlay_state.num_active_pads) * (font_size + spacing)) - spacing));

  // This is a bit of a pain. Some of the glyphs slightly overhang/overshoot past the baseline, resulting
  // in the glyphs getting clipped if we use the text height/margin as a clip point. Instead, just clamp it
  // to the display size, the margin should be enough to allow for overshooting.
  const ImVec4 clip_rect(current_x, current_y, display_size.x - margin, display_size.y);

  SmallString text;

  for (u32 i = 0; i < s_input_overlay_state.num_active_pads; i++)
  {
    const InputOverlayState::PadState& pstate = s_input_overlay_state.pads[i];
    const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(pstate.ctype);
    const char* port_label = Controller::GetPortDisplayName(pstate.port, pstate.slot, pstate.multitap);

    float text_start_x = current_x;
    if (cinfo.icon_name)
    {
      const ImVec2 icon_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, cinfo.icon_name);
      dl->AddText(font, font_size, font_weight, ImVec2(current_x + shadow_offset, current_y + shadow_offset),
                  shadow_color, cinfo.icon_name, nullptr, 0.0f, &clip_rect);
      dl->AddText(font, font_size, font_weight, ImVec2(current_x, current_y), pstate.icon_color, cinfo.icon_name,
                  nullptr, 0.0f, &clip_rect);
      text_start_x += icon_size.x;
      text.format(" {}", port_label);
    }
    else
    {
      text.format("{} |", port_label);
    }

    for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
    {
      if (bi.type >= InputBindingInfo::Type::Button && bi.type <= InputBindingInfo::Type::HalfAxis)
      {
        // axes are always shown when not near resting, buttons only shown when active
        const float value = pstate.bind_state[bi.bind_index];
        const float threshold = (bi.type == InputBindingInfo::Type::Button) ? 0.5f : (254.0f / 255.0f);
        if (value >= threshold)
          text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
        else if (value > (1.0f / 255.0f))
          text.append_format(" {}: {:.2f}", bi.icon_name ? bi.icon_name : bi.name, value);
      }
      else if (bi.type == InputBindingInfo::Type::Motor)
      {
        const float value = pstate.vibration_state[bi.bind_index];
        if (value >= 1.0f)
          text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
        else if (value > 0.0f)
          text.append_format(" {}: {:.0f}%", bi.icon_name ? bi.icon_name : bi.name, std::ceil(value * 100.0f));
      }
    }

    dl->AddText(font, font_size, font_weight, ImVec2(text_start_x + shadow_offset, current_y + shadow_offset),
                shadow_color, text.c_str(), text.end_ptr(), 0.0f, &clip_rect);
    dl->AddText(font, font_size, font_weight, ImVec2(text_start_x, current_y), text_color, text.c_str(), text.end_ptr(),
                0.0f, &clip_rect);

    current_y += font_size + spacing;
  }
}

namespace SaveStateSelectorUI {
namespace {
struct ListEntry
{
  std::string summary;
  std::string game_details; // only in global slots
  std::string filename;
  std::unique_ptr<GPUTexture> preview_texture;
  s32 slot;
  bool global;
};
} // namespace

static void InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global);
static void InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, const std::string& path, s32 slot,
                                bool global);

static void DestroyTextures();
static void RefreshHotkeyLegend();
static void Draw();
static void ShowSlotOSDMessage();
static std::string GetCurrentSlotPath();

static constexpr const char* DATE_TIME_FORMAT =
  TRANSLATE_NOOP("SaveStateSelectorUI", "Saved at {0:%H:%M} on {0:%a} {0:%Y/%m/%d}.");

namespace {

struct ALIGN_TO_CACHE_LINE State
{
  std::shared_ptr<GPUTexture> placeholder_texture;

  std::string load_legend;
  std::string save_legend;
  std::string prev_legend;
  std::string next_legend;

  llvm::SmallVector<ListEntry, System::PER_GAME_SAVE_STATE_SLOTS + System::GLOBAL_SAVE_STATE_SLOTS> slots;
  s32 current_slot = 0;
  bool current_slot_global = false;

  float open_time = 0.0f;
  float close_time = 0.0f;

  ImAnimatedFloat scroll_animated;
  ImAnimatedFloat background_animated;

  bool is_open = false;
};

} // namespace

static State s_state;

} // namespace SaveStateSelectorUI

bool SaveStateSelectorUI::IsOpen()
{
  return s_state.is_open;
}

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
  s_state.open_time = 0.0f;
  s_state.close_time = open_time;

  if (s_state.is_open)
    return;

  if (!s_state.placeholder_texture)
    s_state.placeholder_texture = ImGuiFullscreen::LoadTexture("no-save.png");

  s_state.is_open = true;
  RefreshList();
  RefreshHotkeyLegend();
}

void SaveStateSelectorUI::Close()
{
  s_state.is_open = false;
  s_state.load_legend = {};
  s_state.save_legend = {};
  s_state.prev_legend = {};
  s_state.next_legend = {};
}

void SaveStateSelectorUI::RefreshList()
{
  for (ListEntry& entry : s_state.slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
  s_state.slots.clear();

  const std::string& serial = GPUThread::GetGameSerial();
  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::string path(System::GetGameSaveStatePath(serial, i));
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, false);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, false);

      s_state.slots.push_back(std::move(li));
    }
  }
  else
  {
    // reset slot if it's not global
    if (!s_state.current_slot_global)
    {
      s_state.current_slot = 0;
      s_state.current_slot_global = true;
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::string path(System::GetGlobalSaveStatePath(i));
    std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

    ListEntry li;
    if (ssi)
      InitializeListEntry(&li, &ssi.value(), std::move(path), i, true);
    else
      InitializePlaceholderListEntry(&li, std::move(path), i, true);

    s_state.slots.push_back(std::move(li));
  }
}

void SaveStateSelectorUI::Clear()
{
  // called on CPU thread at shutdown, textures should already be deleted, unless running
  // big picture UI, in which case we have to delete them here...
  ClearList();

  s_state.current_slot = 0;
  s_state.current_slot_global = false;
  s_state.scroll_animated.Reset(0.0f);
  s_state.background_animated.Reset(0.0f);
}

void SaveStateSelectorUI::ClearList()
{
  DebugAssert(GPUThread::IsOnThread());
  for (ListEntry& li : s_state.slots)
  {
    if (li.preview_texture)
      g_gpu_device->RecycleTexture(std::move(li.preview_texture));
  }
  s_state.slots.clear();
}

void SaveStateSelectorUI::DestroyTextures()
{
  Close();

  for (ListEntry& entry : s_state.slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }

  s_state.placeholder_texture.reset();
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  auto format_legend_entry = [](SmallString binding, std::string_view caption) {
    InputManager::PrettifyInputBinding(binding, &ImGuiFullscreen::GetControllerIconMapping);
    return fmt::format("{} - {}", binding, caption);
  };

  s_state.load_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "LoadSelectedSaveState"),
                                            TRANSLATE_SV("SaveStateSelectorUI", "Load"));
  s_state.save_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SaveSelectedSaveState"),
                                            TRANSLATE_SV("SaveStateSelectorUI", "Save"));
  s_state.prev_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SelectPreviousSaveStateSlot"),
                                            TRANSLATE_SV("SaveStateSelectorUI", "Select Previous"));
  s_state.next_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SelectNextSaveStateSlot"),
                                            TRANSLATE_SV("SaveStateSelectorUI", "Select Next"));
}

void SaveStateSelectorUI::SelectNextSlot(bool open_selector)
{
  const s32 total_slots =
    s_state.current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
  s_state.current_slot++;
  if (s_state.current_slot >= total_slots)
  {
    if (!GPUThread::GetGameSerial().empty())
      s_state.current_slot_global ^= true;
    s_state.current_slot -= total_slots;
  }

  if (open_selector)
  {
    if (!s_state.is_open)
      Open();

    s_state.open_time = 0.0f;
  }
  else
  {
    ShowSlotOSDMessage();
  }
}

void SaveStateSelectorUI::SelectPreviousSlot(bool open_selector)
{
  s_state.current_slot--;
  if (s_state.current_slot < 0)
  {
    if (!GPUThread::GetGameSerial().empty())
      s_state.current_slot_global ^= true;
    s_state.current_slot +=
      s_state.current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
  }

  if (open_selector)
  {
    if (!s_state.is_open)
      Open();

    s_state.open_time = 0.0f;
  }
  else
  {
    ShowSlotOSDMessage();
  }
}

void SaveStateSelectorUI::InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, const std::string& path,
                                              s32 slot, bool global)
{
  if (global)
    li->game_details = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "{} ({})"), ssi->title, ssi->serial);

  li->summary = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", DATE_TIME_FORMAT), fmt::localtime(ssi->timestamp));
  li->filename = Path::GetFileName(path);
  li->slot = slot;
  li->global = global;

  // Might not have a display yet, we're called at startup..
  if (g_gpu_device)
  {
    g_gpu_device->RecycleTexture(std::move(li->preview_texture));

    if (ssi->screenshot.IsValid())
    {
      li->preview_texture = g_gpu_device->FetchTexture(
        ssi->screenshot.GetWidth(), ssi->screenshot.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
        GPUTexture::Format::RGBA8, GPUTexture::Flags::None, ssi->screenshot.GetPixels(), ssi->screenshot.GetPitch());
      if (!li->preview_texture) [[unlikely]]
        ERROR_LOG("Failed to upload save state image to GPU");
    }
  }
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global)
{
  li->summary = TRANSLATE_STR("SaveStateSelectorUI", "No save present in this slot.");
  li->slot = slot;
  li->global = global;
}

void SaveStateSelectorUI::Draw()
{
  using ImGuiFullscreen::DarkerColor;
  using ImGuiFullscreen::UIStyle;

  static constexpr float SCROLL_ANIMATION_TIME = 0.25f;
  static constexpr float BG_ANIMATION_TIME = 0.15f;

  const auto& io = ImGui::GetIO();
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = (640.0f * scale);
  const float height = (450.0f * scale);

  const float padding_and_rounding = 18.0f * scale;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, UIStyle.PrimaryColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, UIStyle.BackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, DarkerColor(UIStyle.PopupBackgroundColor));
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);
  ImGui::PushFont(ImGuiManager::GetTextFont(), ImGuiManager::GetOSDFontSize());
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));

  if (ImGui::Begin("##save_state_selector", nullptr,
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar))
  {
    // Leave 2 lines for the legend
    const float legend_margin = ImGui::GetFontSize() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float padding = 12.0f * scale;

    ImGui::BeginChild("##item_list", ImVec2(0, -legend_margin), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoBackground);
    {
      const s32 current_slot = GetCurrentSlot();
      const bool current_slot_global = IsCurrentSlotGlobal();
      const ImVec2 image_size = ImVec2(128.0f * scale, (128.0f / (4.0f / 3.0f)) * scale);
      const float item_width = std::floor(width - (padding_and_rounding * 2.0f) - ImGui::GetStyle().ScrollbarSize);
      const float item_height = std::floor(image_size.y + padding * 2.0f);
      const float text_indent = image_size.x + padding + padding;

      for (size_t i = 0; i < s_state.slots.size(); i++)
      {
        const ListEntry& entry = s_state.slots[i];
        const float y_start = item_height * static_cast<float>(i);

        if (entry.slot == current_slot && entry.global == current_slot_global)
        {
          ImGui::SetCursorPosY(y_start);
          ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);

          const ImVec2 p_start(ImGui::GetCursorScreenPos());
          const ImVec2 p_end(p_start.x + item_width, p_start.y + item_height);
          const ImRect item_rect(p_start, p_end);
          const ImRect& window_rect = ImGui::GetCurrentWindow()->ClipRect;
          if (!window_rect.Contains(item_rect))
          {
            float scroll_target = ImGui::GetScrollY();
            if (item_rect.Min.y < window_rect.Min.y)
              scroll_target = (ImGui::GetScrollY() - (window_rect.Min.y - item_rect.Min.y));
            else if (item_rect.Max.y > window_rect.Max.y)
              scroll_target = (ImGui::GetScrollY() + (item_rect.Max.y - window_rect.Max.y));

            if (scroll_target != s_state.scroll_animated.GetEndValue())
              s_state.scroll_animated.Start(ImGui::GetScrollY(), scroll_target, SCROLL_ANIMATION_TIME);
          }

          if (s_state.scroll_animated.IsActive())
            ImGui::SetScrollY(s_state.scroll_animated.UpdateAndGetValue());

          if (s_state.background_animated.GetEndValue() != p_start.y)
            s_state.background_animated.Start(s_state.background_animated.UpdateAndGetValue(), p_start.y,
                                              BG_ANIMATION_TIME);

          ImVec2 highlight_pos;
          if (s_state.background_animated.IsActive())
            highlight_pos = ImVec2(p_start.x, s_state.background_animated.UpdateAndGetValue());
          else
            highlight_pos = p_start;

          ImGui::GetWindowDrawList()->AddRectFilled(highlight_pos,
                                                    ImVec2(highlight_pos.x + item_width, highlight_pos.y + item_height),
                                                    ImGui::GetColorU32(UIStyle.PrimaryColor), padding_and_rounding);
        }

        if (GPUTexture* preview_texture =
              entry.preview_texture ? entry.preview_texture.get() : s_state.placeholder_texture.get())
        {
          ImGui::SetCursorPosY(y_start + padding);
          ImGui::SetCursorPosX(padding);
          ImGui::Image(preview_texture, image_size);
        }

        ImGui::SetCursorPosY(y_start + padding);

        ImGui::Indent(text_indent);

        ImGui::TextUnformatted(TinyString::from_format(entry.global ?
                                                         TRANSLATE_FS("SaveStateSelectorUI", "Global Slot {}") :
                                                         TRANSLATE_FS("SaveStateSelectorUI", "Game Slot {}"),
                                                       entry.slot)
                                 .c_str());
        if (entry.global)
          ImGui::TextUnformatted(entry.game_details.c_str(), entry.game_details.c_str() + entry.game_details.length());
        ImGui::TextUnformatted(entry.summary.c_str(), entry.summary.c_str() + entry.summary.length());
        ImGui::PushFont(ImGuiManager::GetFixedFont());
        ImGui::TextUnformatted(entry.filename.data(), entry.filename.data() + entry.filename.length());
        ImGui::PopFont();

        ImGui::Unindent(text_indent);
        ImGui::SetCursorPosY(y_start);
        ImGui::ItemSize(ImVec2(item_width, item_height));

        if (entry.slot == current_slot && entry.global == current_slot_global)
          ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##legend", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    {
      ImGui::SetCursorPosX(padding);
      if (ImGui::BeginTable("table", 2))
      {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_state.load_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_state.prev_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_state.save_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_state.next_legend.c_str());

        ImGui::EndTable();
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopFont();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(4);

  // auto-close
  s_state.open_time += io.DeltaTime;
  if (s_state.open_time >= s_state.close_time)
  {
    Close();
  }
  else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
  {
    // Need to cancel the hotkey bindings, otherwise the pause menu will open.
    InputManager::ClearBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
    Close();
  }
}

s32 SaveStateSelectorUI::GetCurrentSlot()
{
  return s_state.current_slot + 1;
}

bool SaveStateSelectorUI::IsCurrentSlotGlobal()
{
  return s_state.current_slot_global;
}

std::string SaveStateSelectorUI::GetCurrentSlotPath()
{
  std::string filename;
  if (!s_state.current_slot_global)
  {
    if (const std::string& serial = GPUThread::GetGameSerial(); !serial.empty())
      filename = System::GetGameSaveStatePath(serial, s_state.current_slot + 1);
  }
  else
  {
    filename = System::GetGlobalSaveStatePath(s_state.current_slot + 1);
  }

  return filename;
}

void SaveStateSelectorUI::LoadCurrentSlot()
{
  DebugAssert(GPUThread::IsOnThread());

  if (std::string path = GetCurrentSlotPath(); !path.empty())
  {
    if (FileSystem::FileExists(path.c_str()))
    {
      Host::RunOnCPUThread([path = std::move(path)]() {
        Error error;
        if (!System::LoadState(path.c_str(), &error, true, false))
        {
          Host::AddKeyedOSDMessage("LoadState",
                                   fmt::format(TRANSLATE_FS("OSDMessage", "Failed to load state from slot {0}:\n{1}"),
                                               GetCurrentSlot(), error.GetDescription()),
                                   Host::OSD_ERROR_DURATION);
        }
      });
    }
    else
    {
      Host::AddIconOSDMessage(
        "LoadState", ICON_EMOJI_FLOPPY_DISK,
        IsCurrentSlotGlobal() ?
          fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "No save state found in Global Slot {}."), GetCurrentSlot()) :
          fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "No save state found in Slot {}."), GetCurrentSlot()),
        Host::OSD_INFO_DURATION);
    }
  }

  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  if (std::string path = GetCurrentSlotPath(); !path.empty())
  {
    Host::RunOnCPUThread([path = std::move(path)]() {
      Error error;
      if (!System::SaveState(std::move(path), &error, g_settings.create_save_state_backups, false))
      {
        Host::AddIconOSDMessage("SaveState", ICON_EMOJI_WARNING,
                                fmt::format(TRANSLATE_FS("OSDMessage", "Failed to save state to slot {0}:\n{1}"),
                                            GetCurrentSlot(), error.GetDescription()),
                                Host::OSD_ERROR_DURATION);
      }
    });
  }

  Close();
}

void SaveStateSelectorUI::ShowSlotOSDMessage()
{
  const std::string path = GetCurrentSlotPath();
  FILESYSTEM_STAT_DATA sd;
  std::string date;
  if (!path.empty() && FileSystem::StatFile(path.c_str(), &sd))
    date = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", DATE_TIME_FORMAT), fmt::localtime(sd.ModificationTime));
  else
    date = TRANSLATE_STR("SaveStateSelectorUI", "no save yet");

  Host::AddIconOSDMessage(
    "ShowSlotOSDMessage", ICON_EMOJI_MAGNIFIYING_GLASS_TILTED_LEFT,
    IsCurrentSlotGlobal() ?
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Global Save Slot {0} selected ({1})."), GetCurrentSlot(), date) :
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Save Slot {0} selected ({1})."), GetCurrentSlot(), date),
    Host::OSD_QUICK_DURATION);
}

void ImGuiManager::RenderOverlayWindows()
{
  const System::State state = System::GetState();
  if (state == System::State::Paused || state == System::State::Running)
  {
    if (SaveStateSelectorUI::s_state.is_open)
      SaveStateSelectorUI::Draw();
  }
}

void ImGuiManager::DestroyOverlayTextures()
{
  SaveStateSelectorUI::DestroyTextures();
}
