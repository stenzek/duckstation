// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_overlays.h"
#include "cdrom.h"
#include "controller.h"
#include "core.h"
#include "cpu_core_private.h"
#include "dma.h"
#include "fullscreenui.h"
#include "fullscreenui_widgets.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gte.h"
#include "host.h"
#include "mdec.h"
#include "performance_counters.h"
#include "settings.h"
#include "spu.h"
#include "system.h"
#include "timers.h"
#include "video_thread.h"

#include "util/gpu_device.h"
#include "util/imgui_animated.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"
#include "util/translation.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
#include "common/time_helpers.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"
#include "fmt/chrono.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <atomic>
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

struct DebugWindowInfo
{
  const char* name;
  const char* window_title;
  const char* icon_name;
  void (*draw_func)(float);
  u32 default_width;
  u32 default_height;
};

} // namespace

static void FormatProcessorStat(SmallStringBase& text, double usage, double time);
static void DrawPerformanceStat(ImDrawList* dl, float& position_y, ImFont* font, float size, float alt_weight,
                                ImU32 alt_color, float rbounds, std::string_view text);
static void DrawStatusIndicators(ImDrawList* dl, float& position_y, ImFont* font, float size, float rbounds,
                                 SmallStringBase& text, bool paused);
static void DrawPerformanceOverlay(const GPUBackend* gpu, float& position_y, float scale, float margin, float spacing);
static void DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawEnhancementsOverlay(const GPUBackend* gpu);
static void DrawInputsOverlay();
static void UpdateInputOverlay(void* buffer);

static constexpr size_t NUM_DEBUG_WINDOWS = 7;
static constexpr const std::array<DebugWindowInfo, NUM_DEBUG_WINDOWS> s_debug_window_info = {{
  {"Freecam", "Free Camera", ":icons/applications-system.png", &GTE::DrawFreecamWindow, 510, 500},
  {"SPU", "SPU State", ":icons/applications-system.png", &SPU::DrawDebugStateWindow, 1100, 745},
  {"CDROM", "CD-ROM State", ":icons/applications-system.png", &CDROM::DrawDebugWindow, 1050, 650},
  {"GPU", "GPU State", ":icons/applications-system.png", &GPU::DrawDebugStateWindow, 1000, 650},
  {"DMA", "DMA State", ":icons/applications-system.png", &DMA::DrawDebugStateWindow, 1160, 215},
  {"MDEC", "MDEC State", ":icons/applications-system.png", &MDEC::DrawDebugStateWindow, 900, 370},
  {"Timers", "Timers State", ":icons/applications-system.png", &Timers::DrawDebugStateWindow, 800, 315},
}};
static std::array<ImGuiManager::AuxiliaryRenderWindowState, NUM_DEBUG_WINDOWS> s_debug_window_state = {};

static InputOverlayState s_input_overlay_state = {};

} // namespace ImGuiManager

u8 ImGuiManager::LoadDebugWindowVisibility(const SettingsInterface& si)
{
  u8 visible = 0;

  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    const DebugWindowInfo& info = s_debug_window_info[i];
    if (si.GetBoolValue(Settings::DEBUG_WINDOWS_SECTION_NAME, info.name, false))
      visible |= (1u << i);
  }

  return visible;
}

void ImGuiManager::SaveDebugWindowVisibility(SettingsInterface& si, u8 debug_window_visibility)
{
  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    const DebugWindowInfo& info = s_debug_window_info[i];
    const bool enabled = ((debug_window_visibility & (1u << i)) != 0);
    si.SetBoolValue(Settings::DEBUG_WINDOWS_SECTION_NAME, info.name, enabled);
  }
}

bool ImGuiManager::IsCDROMDebugWindowVisible(u8 debug_window_visibility)
{
  return ((debug_window_visibility & (1u << 2)) != 0);
}

bool ImGuiManager::IsSPUDebugWindowVisible(u8 debug_window_visibility)
{
  return ((debug_window_visibility & (1u << 1)) != 0);
}

bool ImGuiManager::IsGPUDebugWindowVisible(u8 debug_window_visibility)
{
  return ((debug_window_visibility & (1u << 3)) != 0);
}

bool ImGuiManager::UpdateDebugWindowConfig(u8 debug_window_visibility)
{
  bool was_changed = false;

  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    const DebugWindowInfo& info = s_debug_window_info[i];

    const bool current = (state.window_handle != nullptr);
    const bool enabled = ((debug_window_visibility & (1u << i)) != 0);
    if (enabled == current)
      continue;

    if (!enabled)
    {
      DestroyAuxiliaryRenderWindow(&state, Settings::DEBUG_WINDOWS_SECTION_NAME, info.name);
    }
    else
    {
      Error error;
      if (!CreateAuxiliaryRenderWindow(&state, info.window_title, info.icon_name, Settings::DEBUG_WINDOWS_SECTION_NAME,
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
}

void ImGuiManager::RenderDebugWindows()
{
  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    if (!state.window_handle)
      continue;

    if (!RenderAuxiliaryRenderWindow(&state, s_debug_window_info[i].draw_func))
    {
      // window was closed, destroy it and update the configuration
      const DebugWindowInfo& info = s_debug_window_info[i];
      DestroyAuxiliaryRenderWindow(&state, Settings::DEBUG_WINDOWS_SECTION_NAME, info.name);
      Core::SetBaseBoolSettingValue(Settings::DEBUG_WINDOWS_SECTION_NAME, info.name, false);
      Host::CommitBaseSettingChanges();
    }
  }
}

void ImGuiManager::DestroyAllDebugWindows()
{
  for (size_t i = 0; i < NUM_DEBUG_WINDOWS; i++)
  {
    AuxiliaryRenderWindowState& state = s_debug_window_state[i];
    if (!state.window_handle)
      continue;

    ImGuiManager::DestroyAuxiliaryRenderWindow(&state, Settings::DEBUG_WINDOWS_SECTION_NAME,
                                               s_debug_window_info[i].name);
  }
}

void ImGuiManager::RenderTextOverlays(const GPUBackend* gpu)
{
  // Don't draw anything with loading screen or FSUI open, it'll be nonsensical.
  if (FullscreenUI::IsLoadingScreenOpen() || FullscreenUI::HasActiveOrPendingWindow())
    return;

  const bool paused = VideoThread::IsSystemPaused();

  const float scale = ImGuiManager::GetGlobalScale();
  const float margin = ImGuiManager::GetScreenMargin();
  const float spacing = ImCeil(5.0f * scale);
  float position_y = margin;
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

void ImGuiManager::DrawStatusIndicators(ImDrawList* dl, float& position_y, ImFont* font, float size, float rbounds,
                                        SmallStringBase& text, bool paused)
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

  if (text.empty())
    return;

  constexpr ImU32 color = IM_COL32(255, 255, 255, 255);
  constexpr float default_weight = 0.0f;
  const ImVec2 text_size = font->CalcTextSizeA(size, default_weight, FLT_MAX, 0.0f, IMSTR_START_END(text));

  const ImVec2 position = ImVec2(rbounds - text_size.x, position_y);
  dl->AddText(font, size, default_weight, position, color, IMSTR_START_END(text), 0.0f, nullptr);

  position_y += text_size.y;
}

void ImGuiManager::DrawPerformanceStat(ImDrawList* dl, float& position_y, ImFont* font, float size, float alt_weight,
                                       ImU32 alt_color, float rbounds, std::string_view text)
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
      position.x += FullscreenUI::RenderOutlinedText(dl, font, size, current_weight, position, current_color,
                                                     std::string_view(start_ptr, end_ptr))
                      .x;
    }

    pos = epos;
  } while (pos < text.length());

  position_y += height;
}

void ImGuiManager::DrawPerformanceOverlay(const GPUBackend* gpu, float& position_y, float scale, float margin,
                                          float spacing)
{
#define BOLD(text) "\x02" text "\x01"
#define COLOR(text) "\x04" text "\x03"
#define DOT_SEP " \u2022 "

  if (!(g_gpu_settings.display_show_fps || g_gpu_settings.display_show_speed || g_gpu_settings.display_show_gpu_stats ||
        g_gpu_settings.display_show_resolution || g_gpu_settings.display_show_latency_stats ||
        g_gpu_settings.display_show_cpu_usage || g_gpu_settings.display_show_gpu_usage ||
        g_gpu_settings.display_show_frame_times || g_gpu_settings.display_show_frame_number ||
        (g_gpu_settings.display_show_status_indicators &&
         (VideoThread::IsSystemPaused() || System::IsFastForwardEnabled() || System::IsTurboEnabled()))))
  {
    return;
  }

  const float status_size = std::ceil(50.0f * scale);
  ImFont* const fixed_font = ImGuiManager::GetFixedFont();
  const float fixed_font_size = ImGuiManager::GetFixedFontSize();
  ImFont* const ui_font = ImGuiManager::GetTextFont();
  const float rbound = ImGui::GetIO().DisplaySize.x - margin;
  ImDrawList* const dl = ImGui::GetBackgroundDrawList();
  SmallString text;

  const bool paused = VideoThread::IsSystemPaused();
  if (!paused)
  {
    const float speed = PerformanceCounters::GetEmulationSpeed();
    if (g_gpu_settings.display_show_fps)
    {
      const float fps = PerformanceCounters::GetFPS();
      const float vps = PerformanceCounters::GetVPS();

      if (vps < 100.0f)
        text.append_format(COLOR("{:.2f}") " " BOLD("FPS") DOT_SEP COLOR("{:.2f}") " " BOLD("VPS"), fps, vps);
      else if (vps < 1000.0f)
        text.append_format(COLOR("{:.1f}") " " BOLD("FPS") DOT_SEP COLOR("{:.1f}") " " BOLD("VPS"), fps, vps);
      else
        text.append_format(COLOR("{:.0f}") " " BOLD("FPS") DOT_SEP COLOR("{:.0f}") " " BOLD("VPS"), fps, vps);
    }
    if (g_gpu_settings.display_show_speed)
    {
      text.append_format("{}" COLOR("{}%") " ", text.empty() ? "" : DOT_SEP, static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.append(BOLD("(Max)"));
      else
        text.append_format(BOLD("({:.0f}%)"), target_speed * 100.0f);
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

      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, alt_color, rbound, text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_gpu_stats)
    {
      gpu->GetStatsString(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
      position_y += spacing;

      gpu->GetMemoryStatsString(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
      position_y += spacing;
    }
  }

  if (g_gpu_settings.display_show_resolution)
  {
    const u32 resolution_scale = gpu->GetResolutionScale();
    const bool pgxp = gpu->IsUsingHardwareBackend() && g_gpu_settings.gpu_pgxp_enable;
    const auto [display_width, display_height] = GPU::GetFullDisplayResolution(); // NOTE: Racey read.
    const bool interlaced = GPU::IsInterlacedDisplayEnabled();
    const bool progressive_forced = GPU::IsProgressiveDisplayScanForced();
    const bool pal = GPU::IsInPALMode();
    text.format("{}x{} " BOLD("{} {}") DOT_SEP "{}x " BOLD("IR") "{}", display_width * resolution_scale,
                display_height * resolution_scale, pal ? "PAL" : "NTSC",
                interlaced ? "Interlaced" : (progressive_forced ? "Forced-Progressive" : "Progressive"),
                resolution_scale, pgxp ? DOT_SEP BOLD("PGXP") : "");
    DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
    position_y += spacing;
  }

  if (!paused)
  {
    if (g_gpu_settings.display_show_latency_stats)
    {
      System::FormatLatencyStats(text);
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
      position_y += spacing;
    }

    if (g_gpu_settings.display_show_cpu_usage)
    {
      text.format(" {:.2f}ms " BOLD("Min") DOT_SEP "{:.2f}ms " BOLD("Avg") DOT_SEP "{:.2f}ms " BOLD("Max"),
                  PerformanceCounters::GetMinimumFrameTime(), PerformanceCounters::GetAverageFrameTime(),
                  PerformanceCounters::GetMaximumFrameTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
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

        text.append("]:\x01 ");
      }
      else
      {
        text.assign(BOLD("CPU:") " ");
      }
      FormatProcessorStat(text, PerformanceCounters::GetCoreThreadUsage(),
                          PerformanceCounters::GetCoreThreadAverageTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
      position_y += spacing;

      if (g_gpu_settings.gpu_use_thread)
      {
        text.assign(BOLD("RNDR:") " ");
        FormatProcessorStat(text, PerformanceCounters::GetVideoThreadUsage(),
                            PerformanceCounters::GetVideoThreadAverageTime());
        DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
        position_y += spacing;
      }

      if (MediaCapture* cap = System::GetMediaCapture())
      {
        text.assign(BOLD("CAP:") " ");
        FormatProcessorStat(text, cap->GetCaptureThreadUsage(), cap->GetCaptureThreadTime());
        DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
        position_y += spacing;
      }
    }

    if (g_gpu_settings.display_show_gpu_usage && g_gpu_device->IsGPUTimingEnabled())
    {
      text.assign(BOLD("GPU:") " ");
      FormatProcessorStat(text, PerformanceCounters::GetGPUUsage(), PerformanceCounters::GetGPUAverageTime());
      DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
      position_y += spacing;
    }
  }

  if (g_gpu_settings.display_show_frame_number)
  {
    gpu->GetFrameNumberString(text);
    DrawPerformanceStat(dl, position_y, fixed_font, fixed_font_size, FIXED_BOLD_WEIGHT, 0, rbound, text);
    position_y += spacing;
    text.clear();
  }

  if (!paused && g_gpu_settings.display_show_frame_times)
    DrawFrameTimeOverlay(position_y, scale, margin, spacing);

  if (g_gpu_settings.display_show_status_indicators)
    DrawStatusIndicators(dl, position_y, ui_font, status_size, rbound, text, paused);

#undef DOT_SEP
#undef COLOR
#undef BOLD
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
    text.append_format(" RA={}{}", g_settings.runahead_frames, g_settings.runahead_for_analog_input ? "+A" : "");

  if (g_settings.cpu_overclock_active)
    text.append_format(" CPU={}%", g_settings.GetCPUOverclockPercent());
  if (g_settings.cpu_ram_size != Settings::DEFAULT_CPU_RAM_SIZE)
    text.append_format(" {}", Settings::GetCPURAMSizeName(g_settings.cpu_ram_size));
  if (g_settings.gpu_vram_size != Settings::DEFAULT_GPU_VRAM_SIZE)
    text.append_format(" VRAM={}", Settings::GetGPUVRAMSizeName(g_settings.gpu_vram_size));
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
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto() &&
      g_settings.display_aspect_ratio != DisplayAspectRatio{4, 3})
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

  const float margin = ImGuiManager::GetScreenMargin();
  ImFont* const font = ImGuiManager::GetFixedFont();
  const float font_size = ImGuiManager::GetFixedFontSize();
  const float font_weight = 600.0f;
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - font_size;

  const ImVec2 text_size = font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), -1.0f,
                                               text.c_str(), text.end_ptr(), nullptr);
  FullscreenUI::RenderOutlinedText(ImGui::GetBackgroundDrawList(), font, font_size, font_weight,
                                   ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
                                   IM_COL32(255, 255, 255, 255), text);
}

void ImGuiManager::DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing)
{
  MediaCapture* const cap = System::GetMediaCapture();
  if (!cap)
    return;

  const float shadow_offset = std::ceil(scale);
  ImFont* const font = ImGuiManager::GetTextFont();
  const float font_size = ImGuiManager::GetOSDFontSize();
  static constexpr const float& font_weight = FullscreenUI::UIStyle.BoldFontWeight;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  static constexpr const char* ICON = ICON_PF_CIRCLE;
  const time_t elapsed_time = cap->GetElapsedTime();
  const TinyString text_msg =
    TinyString::from_format(" {:02d}:{:02d}:{:02d}  ({})", elapsed_time / 3600, (elapsed_time % 3600) / 60,
                            (elapsed_time % 3600) % 60, Host::FormatByteSize(cap->GetOutputSize()));
  const ImVec2 icon_size =
    font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), -1.0f, ICON, nullptr, nullptr);
  const ImVec2 text_size = font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), -1.0f,
                                               text_msg.c_str(), text_msg.end_ptr(), nullptr);

  const float box_margin = 5.0f * scale;
  const ImVec2 box_size = ImVec2(ImCeil(icon_size.x + shadow_offset + text_size.x + box_margin * 2.0f),
                                 ImCeil(std::max(icon_size.y, text_size.y) + box_margin * 2.0f));
  const ImVec2 box_pos = ImVec2(ImGui::GetIO().DisplaySize.x - margin - box_size.x, position_y);
  dl->AddRectFilled(box_pos, box_pos + box_size, IM_COL32(0, 0, 0, 64), box_margin);

  const ImVec2 text_start = ImVec2(box_pos.x + box_margin, box_pos.y + box_margin);
  dl->AddText(font, font_size, font_weight, ImVec2(text_start.x + shadow_offset, text_start.y + shadow_offset),
              IM_COL32(0, 0, 0, 100), ICON);
  dl->AddText(font, font_size, font_weight,
              ImVec2(text_start.x + icon_size.x + shadow_offset, text_start.y + shadow_offset), IM_COL32(0, 0, 0, 100),
              text_msg.c_str(), text_msg.end_ptr());
  dl->AddText(font, font_size, font_weight, text_start, IM_COL32(255, 0, 0, 255), ICON);
  dl->AddText(font, font_size, font_weight, ImVec2(text_start.x + icon_size.x, text_start.y),
              IM_COL32(255, 255, 255, 255), text_msg.c_str(), text_msg.end_ptr());

  position_y += box_size.y + spacing;
}

void ImGuiManager::DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing)
{
  constexpr ImU32 text_color = IM_COL32(240, 240, 240, 255);
  constexpr ImU32 line_color = IM_COL32(240, 240, 240, 255);
  const float shadow_offset = OSDScale(1.0f);
  const float font_size = OSDScale(13.0f);
  static constexpr ImFont*& font = FullscreenUI::UIStyle.Font;
  static constexpr const float& font_weight = FullscreenUI::UIStyle.BoldFontWeight;

  const ImVec2 window_padding = ImVec2(OSDScale(8.0f), OSDScale(4.0f));
  const ImVec2 window_size = ImVec2(OSDScale(200.0f), OSDScale(40.0f));
  const ImVec2 padded_window_size = window_size + (window_padding * 2.0f);
  ImGui::SetNextWindowSize(padded_window_size);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - margin - padded_window_size.x, position_y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_PlotLines, line_color);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, OSDScale(8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, window_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  if (ImGui::Begin("##frame_times", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    // LLVM likes to unroll this... whatever.
    float min, max;
    {
      const PerformanceCounters::FrameTimeHistory& history = PerformanceCounters::GetFrameTimeHistory();
      static_assert((PerformanceCounters::NUM_FRAME_TIME_SAMPLES % 4) == 0);
      GSVector4 vmin = GSVector4::load<true>(history.data());
      GSVector4 vmax = vmin;
      for (size_t i = 4; i < history.size(); i += 4)
      {
        const GSVector4 v = GSVector4::load<true>(&history[i]);
        vmin = vmin.min(v);
        vmax = vmax.max(v);
      }

      min = vmin.minv();
      max = vmax.maxv();
    }

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
      nullptr, PerformanceCounters::NUM_FRAME_TIME_SAMPLES, 0, nullptr, min, max, window_size);

    ImGuiWindow* const win = ImGui::GetCurrentWindow();
    ImDrawList* const win_dl = win->DrawList;

    TinyString text;
    text.format("{:.0f} ms", max);
    ImVec2 text_size = font->CalcTextSizeA(font_size, -1.0f, FLT_MAX, 0.0f, IMSTR_START_END(text));
    FullscreenUI::RenderShadowedTextClipped(win_dl, font, font_size, font_weight, win->WorkRect.Min, win->WorkRect.Max,
                                            text_color, text, &text_size, ImVec2(1.0f, 0.0f), 0.0f, &win->WorkRect,
                                            shadow_offset);

    text.format("{:.0f} ms", min);
    text_size = font->CalcTextSizeA(font_size, -1.0f, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
    FullscreenUI::RenderShadowedTextClipped(win_dl, font, font_size, font_weight, win->WorkRect.Min, win->WorkRect.Max,
                                            text_color, text, &text_size, ImVec2(1.0f, 1.0f), 0.0f, &win->WorkRect,
                                            shadow_offset);
  }
  ImGui::End();
  ImGui::PopStyleVar(5);
  ImGui::PopStyleColor(3);

  position_y += padded_window_size.y + spacing;
}

void ImGuiManager::UpdateInputOverlay()
{
  static constexpr u32 NORMAL_ICON_COLOR = 0xFFCCCCCCu;
  static constexpr u32 ALTERNATE_ICON_COLOR = 0xFF2534F0u;

  u32 num_active_pads = 0;
  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] != ControllerType::None)
      num_active_pads++;
  }

  const u32 buffer_size =
    sizeof(InputOverlayStateUpdateBuffer) + (static_cast<u32>(sizeof(InputOverlayState::PadState)) * num_active_pads);
  const auto& [cmd, buffer] = VideoThread::BeginASyncBufferCall(&ImGuiManager::UpdateInputOverlay, buffer_size);

  InputOverlayStateUpdateBuffer* const ubuffer = static_cast<InputOverlayStateUpdateBuffer*>(buffer);
  ubuffer->num_active_pads = num_active_pads;

  size_t out_index = 0;
  const std::array<bool, 2> multitap_ports = Controller::GetMultitapEnabledPorts(g_settings.multitap_mode);
  for (const u32 pad : Controller::PortDisplayOrder)
  {
    const Controller* controller = System::GetController(pad);
    if (!controller)
      continue;

    const ControllerType ctype = controller->GetType();
    const auto& [port, slot] = Controller::ConvertPadToPortAndSlot(pad);
    InputOverlayState::PadState& pstate = ubuffer->pads[out_index++];
    pstate.port = Truncate8(port);
    pstate.slot = Truncate8(slot);
    pstate.multitap = multitap_ports[port];
    pstate.ctype = ctype;
    pstate.icon_color = NORMAL_ICON_COLOR;

    const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ctype);
    for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
    {
      const u32 bidx = bi.bind_index;

      // this will leave some uninitalized, but who cares, it won't be read on the other side
      if (bi.type >= InputBindingInfo::Type::Button && bi.type <= InputBindingInfo::Type::Motor)
      {
        DebugAssert(bidx < InputOverlayState::MAX_BINDS);
        pstate.bind_state[bidx] = controller->GetBindState(bidx);
      }
      else if (bi.type == InputBindingInfo::Type::LED)
      {
        const float intensity = controller->GetBindState(bidx);
        pstate.icon_color = (GSVector4::cxpr_rgba32(NORMAL_ICON_COLOR) + (GSVector4::cxpr_rgba32(ALTERNATE_ICON_COLOR) -
                                                                          GSVector4::cxpr_rgba32(NORMAL_ICON_COLOR)) *
                                                                           GSVector4(intensity))
                              .rgba32();
      }
    }
  }

  VideoThread::EndASyncBufferCall(cmd);
}

void ImGuiManager::UpdateInputOverlay(void* buffer)
{
  InputOverlayStateUpdateBuffer* const RESTRICT ubuffer = static_cast<InputOverlayStateUpdateBuffer*>(buffer);
  DebugAssert(ubuffer->num_active_pads <= NUM_CONTROLLER_AND_CARD_PORTS);
  s_input_overlay_state.num_active_pads = ubuffer->num_active_pads;
  for (u32 i = 0; i < ubuffer->num_active_pads; i++)
    s_input_overlay_state.pads[i] = ubuffer->pads[i];
}

void ImGuiManager::DrawInputsOverlay()
{
  const float scale = ImGuiManager::GetGlobalScale();
  const float margin = ImGuiManager::GetScreenMargin();
  const float spacing = ImCeil(5.0f * scale);
  ImFont* const font = ImGuiManager::GetTextFont();
  const float font_size = ImGuiManager::GetOSDFontSize();
  const float font_weight = 400.0f;

  static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);

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
      const ImVec2 icon_size = FullscreenUI::RenderOutlinedText(
        dl, font, font_size, font_weight, ImVec2(current_x, current_y), pstate.icon_color, cinfo.icon_name);
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
        const float value = pstate.bind_state[bi.bind_index];
        if (value >= 1.0f)
          text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
        else if (value > 0.0f)
          text.append_format(" {}: {:.0f}%", bi.icon_name ? bi.icon_name : bi.name, std::ceil(value * 100.0f));
      }
    }

    FullscreenUI::RenderOutlinedText(dl, font, font_size, font_weight, ImVec2(text_start_x, current_y), text_color,
                                     text);

    current_y += font_size + spacing;
  }
}

namespace SaveStateSelectorUI {
namespace {
struct ListEntry
{
  std::string summary;
  std::string game_details; // only in global slots
  std::string date;         // empty for empty slots
  std::unique_ptr<GPUTexture> preview_texture;
  s32 slot;
  bool global;
};
} // namespace

static void InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global, bool exists,
                                           Error&& error);
static void InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, const std::string& path, s32 slot,
                                bool global);

static void DestroyTextures();
static void RefreshHotkeyLegend();
static void CloseImmediately();
static void Draw();
static void ShowSlotOSDMessage();

static constexpr float OPEN_ANIMATION_TIME = 0.2f;
static constexpr float CLOSE_ANIMATION_TIME = 0.1f;

namespace {

struct State
{
  std::string load_legend;
  std::string save_legend;
  std::string prev_legend;
  std::string next_legend;

  llvm::SmallVector<ListEntry, System::PER_GAME_SAVE_STATE_SLOTS + System::GLOBAL_SAVE_STATE_SLOTS> slots;
  s32 current_slot = 0;
  bool current_slot_global = false;
  bool global_slots_enabled = false;

  float open_time = 0.0f;
  float close_time = 0.0f;
  float animation_time_remaining = 0.0f;

  ImAnimatedFloat scroll_animated;
  ImAnimatedFloat background_animated;
  float preview_fade = 1.0f;
  size_t preview_index = std::numeric_limits<size_t>::max();
  size_t preview_prev_index = std::numeric_limits<size_t>::max();

  bool is_open = false;
  bool is_closing = false;
};

} // namespace

ALIGN_TO_CACHE_LINE static State s_state;

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
  {
    // Abort close.
    if (s_state.is_closing)
    {
      s_state.animation_time_remaining = 0.0f;
      s_state.is_closing = false;
    }

    return;
  }

  s_state.is_open = true;
  s_state.is_closing = false;
  s_state.animation_time_remaining = FullscreenUI::UIStyle.Animations ? OPEN_ANIMATION_TIME : 0.0f;
  s_state.global_slots_enabled = System::AreGlobalSaveStatesEnabled();
  RefreshList();
  RefreshHotkeyLegend();

  if (s_state.slots.empty())
  {
    FullscreenUI::ShowToast(OSDMessageType::Info, {},
                            TRANSLATE_STR("SaveStateSelectorUI", "No save state slots available."));
    CloseImmediately();
  }
}

void SaveStateSelectorUI::Close()
{
  if (!s_state.is_open || s_state.is_closing)
    return;

  s_state.animation_time_remaining = FullscreenUI::UIStyle.Animations ? CLOSE_ANIMATION_TIME : 0.0f;
  s_state.is_closing = true;
  if (s_state.animation_time_remaining <= 0.0f)
    CloseImmediately();
}

void SaveStateSelectorUI::CloseImmediately()
{
  ClearList();
  s_state.is_open = false;
  s_state.is_closing = false;
  s_state.animation_time_remaining = 0.0f;
  s_state.preview_index = std::numeric_limits<size_t>::max();
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

  const std::string& serial = VideoThread::GetGameSerial();
  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      Error error;
      bool exists;
      std::string path = System::GetGameSaveStatePath(serial, i);
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str(), &error, &exists);

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, false);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, false, exists, std::move(error));

      s_state.slots.push_back(std::move(li));
    }
  }

  if (s_state.global_slots_enabled)
  {
    // reset slot if it's not global
    if (serial.empty() && !s_state.current_slot_global)
    {
      s_state.current_slot = 0;
      s_state.current_slot_global = true;
    }

    for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      Error error;
      bool exists;
      std::string path = System::GetGlobalSaveStatePath(i);
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str(), &error, &exists);

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, true);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, true, exists, std::move(error));

      s_state.slots.push_back(std::move(li));
    }
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
  DebugAssert(VideoThread::IsOnThread());
  for (ListEntry& li : s_state.slots)
  {
    if (li.preview_texture)
      g_gpu_device->RecycleTexture(std::move(li.preview_texture));
  }
  s_state.slots.clear();
}

void SaveStateSelectorUI::DestroyTextures()
{
  // We don't cache the screenshots in memory, so just close completely and let it repopulate on open.
  CloseImmediately();
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  static constexpr auto format_legend_entry = [](SmallString binding, std::string_view caption) {
    InputManager::PrettifyInputBinding(binding, true, &FullscreenUI::GetControllerIconMapping);
    return fmt::format("{}  {}", binding, caption);
  };

  s_state.load_legend =
    format_legend_entry(Core::GetSmallStringSettingValue(Settings::HOTKEYS_SECTION_NAME, "LoadSelectedSaveState"),
                        TRANSLATE_SV("SaveStateSelectorUI", "Load"));
  s_state.save_legend =
    format_legend_entry(Core::GetSmallStringSettingValue(Settings::HOTKEYS_SECTION_NAME, "SaveSelectedSaveState"),
                        TRANSLATE_SV("SaveStateSelectorUI", "Save"));
  s_state.prev_legend =
    format_legend_entry(Core::GetSmallStringSettingValue(Settings::HOTKEYS_SECTION_NAME, "SelectPreviousSaveStateSlot"),
                        TRANSLATE_SV("SaveStateSelectorUI", "Select Previous"));
  s_state.next_legend =
    format_legend_entry(Core::GetSmallStringSettingValue(Settings::HOTKEYS_SECTION_NAME, "SelectNextSaveStateSlot"),
                        TRANSLATE_SV("SaveStateSelectorUI", "Select Next"));
}

void SaveStateSelectorUI::SelectNextSlot(bool open_selector)
{
  const s32 total_slots =
    s_state.current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
  s_state.current_slot++;
  if (s_state.current_slot >= total_slots)
  {
    if (s_state.global_slots_enabled && !VideoThread::GetGameSerial().empty())
      s_state.current_slot_global ^= true;
    s_state.current_slot -= total_slots;
  }

  if (open_selector)
  {
    if (!s_state.is_open)
      Open();
    else
    {
      s_state.open_time = 0.0f; // stay open for the full duration
      if (s_state.is_closing)
      {
        s_state.animation_time_remaining = 0.0f;
        s_state.is_closing = false;
      }
    }
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
    if (s_state.global_slots_enabled && !VideoThread::GetGameSerial().empty())
      s_state.current_slot_global ^= true;
    s_state.current_slot +=
      s_state.current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
  }

  if (open_selector)
  {
    if (!s_state.is_open)
      Open();
    else
    {
      s_state.open_time = 0.0f; // stay open for the full duration
      if (s_state.is_closing)
      {
        s_state.animation_time_remaining = 0.0f;
        s_state.is_closing = false;
      }
    }
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

  li->summary = Host::FormatRelativeDateTime(ssi->timestamp, false, true);
  li->date = Host::FormatDateTime(ssi->timestamp, false);
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
        GPUTextureFormat::RGBA8, GPUTexture::Flags::None, ssi->screenshot.GetPixels(), ssi->screenshot.GetPitch());
      if (!li->preview_texture) [[unlikely]]
        ERROR_LOG("Failed to upload save state image to GPU");
    }
  }
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global,
                                                         bool exists, Error&& error)
{
  if (!exists)
    li->summary = TRANSLATE_STR("SaveStateSelectorUI", "Empty");
  else if (error.IsValid())
    li->summary = error.TakeDescription();
  else
    li->summary = TRANSLATE_STR("SaveStateSelectorUI", "Unknown error.");
  li->slot = slot;
  li->global = global;
}

void SaveStateSelectorUI::Draw()
{
  using FullscreenUI::DarkerColor;
  using FullscreenUI::LayoutScale;
  using FullscreenUI::ModAlpha;
  using FullscreenUI::RenderShadowedTextClipped;
  using FullscreenUI::UIStyle;

  static constexpr float SCROLL_ANIMATION_TIME = 0.25f;
  static constexpr float BG_ANIMATION_TIME = 0.15f;
  static constexpr float PREVIEW_FADE_TIME = 0.25f;

  const auto& io = ImGui::GetIO();
  const float width = LayoutScale(880.0f);
  const float height = LayoutScale(500.0f);

  float alpha = 1.0f;
  float position_offset = 0.0f;
  if (s_state.animation_time_remaining > 0.0f)
  {
    s_state.animation_time_remaining -= io.DeltaTime;
    if (s_state.animation_time_remaining <= 0.0f)
    {
      s_state.animation_time_remaining = 0.0f;
      if (s_state.is_closing)
      {
        CloseImmediately();
        return;
      }
    }
    else if (s_state.is_closing)
    {
      const float fract = s_state.animation_time_remaining / CLOSE_ANIMATION_TIME;
      alpha = fract;
      position_offset = ImCeil(LayoutScale(20.0f) * (1.0f - fract));
    }
    else
    {
      const float fract = Easing::InExpo(s_state.animation_time_remaining / OPEN_ANIMATION_TIME);
      alpha = 1.0f - fract;
      position_offset = ImCeil(LayoutScale(50.0f) * fract);
    }
  }

  const float padding = LayoutScale(20.0f);
  const float window_rounding = LayoutScale(18.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, window_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, DarkerColor(UIStyle.PopupBackgroundColor));
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f + position_offset), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));

  if (FullscreenUI::BeginBlurWindow("##save_state_selector", nullptr,
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar,
                                    UIStyle.BlurMenuBackground))
  {
    ImFont* const font = UIStyle.Font;
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    const ImVec2 content_min = ImGui::GetCursorScreenPos();
    const ImRect content(content_min, content_min + ImGui::GetContentRegionAvail());
    const float spacing = LayoutScale(4.0f);
    const u32 text_color = ImGui::GetColorU32(UIStyle.BackgroundTextColor);
    const u32 muted_color = ImGui::GetColorU32(UIStyle.MutedTextColor);
    const u32 accent_color = ImGui::GetColorU32(UIStyle.AccentColor);

    const s32 current_slot = GetCurrentSlot();
    const bool current_slot_global = IsCurrentSlotGlobal();
    size_t selected_index = 0;
    u32 slots_in_use = 0;
    for (size_t i = 0; i < s_state.slots.size(); i++)
    {
      const ListEntry& entry = s_state.slots[i];
      slots_in_use += BoolToUInt32(!entry.date.empty());
      if (entry.slot == current_slot && entry.global == current_slot_global)
        selected_index = i;
    }

    static constexpr auto slot_title = [](const ListEntry& entry) {
      return TinyString::from_format(entry.global ? TRANSLATE_FS("SaveStateSelectorUI", "Global Slot {}") :
                                                    TRANSLATE_FS("SaveStateSelectorUI", "Game Slot {}"),
                                     entry.slot);
    };
    static constexpr auto draw_thumbnail = [](ImDrawList* dl, const ListEntry& entry, const ImRect& bb, float rounding,
                                              bool backing, float alpha) {
      if (!entry.preview_texture)
      {
        FullscreenUI::DrawEmptySlotTile(dl, bb, rounding, alpha);
        return;
      }

      if (backing)
        dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, alpha)), rounding);

      const ImRect image_bb = FullscreenUI::CenterImage(bb, entry.preview_texture.get());
      dl->AddImageRounded(entry.preview_texture.get(), image_bb.Min, image_bb.Max, ImVec2(0.0f, 0.0f),
                          ImVec2(1.0f, 1.0f), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), rounding);
    };

    // Header: title and slot usage.
    const std::string_view title = TRANSLATE_SV("SaveStateSelectorUI", "Save States");
    const ImRect header_bb(content.Min, ImVec2(content.Max.x, content.Min.y + UIStyle.MediumLargeFontSize));
    const ImVec2 title_size =
      font->CalcTextSizeA(UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f, IMSTR_START_END(title));
    RenderShadowedTextClipped(dl, font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, header_bb.Min,
                              header_bb.Max, text_color, title, &title_size);
    RenderShadowedTextClipped(dl, font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight,
                              ImVec2(header_bb.Min.x + title_size.x + padding, header_bb.Min.y), header_bb.Max,
                              muted_color,
                              SmallString::from_format(TRANSLATE_FS("SaveStateSelectorUI", "{} of {} slots in use"),
                                                       slots_in_use, s_state.slots.size()),
                              nullptr, ImVec2(1.0f, 0.5f));

    // Footer: hotkey legend, with the auto-close countdown above it.
    const ImRect footer_bb(ImVec2(content.Min.x, content.Max.y - UIStyle.MediumFontSize), content.Max);
    const std::array<const std::string*, 4> legends = {&s_state.load_legend, &s_state.save_legend, &s_state.prev_legend,
                                                       &s_state.next_legend};
    std::array<ImVec2, 4> legend_sizes;
    float legend_gap = footer_bb.GetWidth();
    for (size_t i = 0; i < legends.size(); i++)
    {
      legend_sizes[i] = font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                            IMSTR_START_END(*legends[i]));
      legend_gap -= legend_sizes[i].x;
    }
    legend_gap = std::max(legend_gap / static_cast<float>(legends.size() - 1), padding);
    float legend_x = footer_bb.Min.x;
    for (size_t i = 0; i < legends.size(); i++)
    {
      RenderShadowedTextClipped(dl, font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight,
                                ImVec2(legend_x, footer_bb.Min.y), footer_bb.Max, muted_color, *legends[i],
                                &legend_sizes[i]);
      legend_x += legend_sizes[i].x + legend_gap;
    }

    const float bar_height = LayoutScale(3.0f);
    const ImRect bar_bb(ImVec2(content.Min.x, footer_bb.Min.y - padding - bar_height),
                        ImVec2(content.Max.x, footer_bb.Min.y - padding));
    const float remaining =
      s_state.is_closing ? 0.0f : std::clamp(1.0f - (s_state.open_time / s_state.close_time), 0.0f, 1.0f);
    dl->AddRectFilled(bar_bb.Min, bar_bb.Max, ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundLineColor, 0.1f)),
                      bar_height);
    dl->AddRectFilled(bar_bb.Min, ImVec2(bar_bb.Min.x + bar_bb.GetWidth() * remaining, bar_bb.Max.y),
                      ImGui::GetColorU32(ModAlpha(UIStyle.AccentColor, 0.8f)), bar_height);

    // Body: large preview of the selected slot on the left, slot list on the right.
    const ImRect body_bb(ImVec2(content.Min.x, header_bb.Max.y + padding),
                         ImVec2(content.Max.x, bar_bb.Min.y - padding));
    const float preview_width = LayoutScale(420.0f);
    if (!s_state.slots.empty())
    {
      const ListEntry& entry = s_state.slots[selected_index];
      const bool show_game = (entry.global && !entry.game_details.empty());
      const float details_height = UIStyle.MediumLargeFontSize + spacing + UIStyle.MediumFontSize +
                                   (show_game ? (spacing + UIStyle.MediumFontSize) : 0.0f);
      const float image_height =
        std::min(std::floor(preview_width * 0.75f), body_bb.GetHeight() - details_height - padding * 0.5f);
      const ImRect image_bb(body_bb.Min, ImVec2(body_bb.Min.x + preview_width, body_bb.Min.y + image_height));

      // Crossfade from the previously selected slot's preview.
      if (selected_index != s_state.preview_index)
      {
        s_state.preview_prev_index = s_state.preview_index;
        s_state.preview_index = selected_index;
        s_state.preview_fade = (UIStyle.Animations && s_state.preview_prev_index < s_state.slots.size()) ? 0.0f : 1.0f;
      }
      s_state.preview_fade = std::min(s_state.preview_fade + io.DeltaTime / PREVIEW_FADE_TIME, 1.0f);

      const float fade = s_state.preview_fade;
      const float rounding = LayoutScale(10.0f);
      const ListEntry* const prev_entry = (fade < 1.0f && s_state.preview_prev_index < s_state.slots.size()) ?
                                            &s_state.slots[s_state.preview_prev_index] :
                                            nullptr;
      const bool new_has_image = static_cast<bool>(entry.preview_texture);
      const bool old_has_image = (prev_entry && prev_entry->preview_texture);

      // The letterbox backing is drawn once under both, and the old screenshot stays opaque under the new one, so
      // two screenshots blend linearly. The empty tile is translucent, so fading to or from one fades both sides.
      const float backing =
        (new_has_image ? (prev_entry ? fade : 1.0f) : 0.0f) + (old_has_image ? (1.0f - fade) : 0.0f);
      if (backing > 0.0f)
        dl->AddRectFilled(image_bb.Min, image_bb.Max, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, backing)), rounding);
      if (prev_entry)
      {
        draw_thumbnail(dl, *prev_entry, image_bb, rounding, false,
                       (old_has_image && new_has_image) ? 1.0f : (1.0f - fade));
      }
      draw_thumbnail(dl, entry, image_bb, rounding, false, prev_entry ? fade : 1.0f);

      float text_y = image_bb.Max.y + padding * 0.5f;
      RenderShadowedTextClipped(dl, font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight,
                                ImVec2(image_bb.Min.x, text_y), ImVec2(image_bb.Max.x, body_bb.Max.y), text_color,
                                slot_title(entry));
      text_y += UIStyle.MediumLargeFontSize + spacing;
      if (show_game)
      {
        RenderShadowedTextClipped(dl, font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight,
                                  ImVec2(image_bb.Min.x, text_y), ImVec2(image_bb.Max.x, body_bb.Max.y), text_color,
                                  entry.game_details);
        text_y += UIStyle.MediumFontSize + spacing;
      }
      RenderShadowedTextClipped(
        dl, font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, ImVec2(image_bb.Min.x, text_y),
        ImVec2(image_bb.Max.x, body_bb.Max.y), muted_color,
        (entry.date.empty() || entry.date == entry.summary) ?
          SmallString(entry.summary) :
          SmallString::from_format("{} " ICON_EMOJI_MIDDLE_DOT " {}", entry.summary, entry.date));
    }

    const ImRect list_bb(ImVec2(body_bb.Min.x + preview_width + padding, body_bb.Min.y), body_bb.Max);
    ImGui::SetCursorScreenPos(list_bb.Min);
    ImGui::BeginChild("##item_list", list_bb.GetSize(), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    {
      ImGuiWindow* const window = ImGui::GetCurrentWindow();
      ImDrawList* const list_dl = window->DrawList;
      const float row_padding = LayoutScale(8.0f);
      const float row_rounding = LayoutScale(10.0f);
      const ImVec2 thumb_size = LayoutScale(64.0f, 48.0f);
      const float row_width = list_bb.GetWidth();
      const float row_height = thumb_size.y + row_padding * 2.0f;
      const ImVec2 origin = window->DC.CursorPos;

      // Keep the selected row in view and clear of the edge fade. Positions are relative to the list origin so
      // scrolling doesn't restart the highlight animation.
      const float selected_y = row_height * static_cast<float>(selected_index);
      const float fade_margin = LayoutScale(FullscreenUI::LAYOUT_SCROLL_FADE_HEIGHT);
      const ImRect selected_bb(ImVec2(origin.x, origin.y + selected_y - fade_margin),
                               ImVec2(origin.x + row_width, origin.y + selected_y + row_height + fade_margin));
      const ImRect& clip_rect = window->ClipRect;
      if (!clip_rect.Contains(selected_bb))
      {
        float scroll_target = ImGui::GetScrollY();
        if (selected_bb.Min.y < clip_rect.Min.y)
          scroll_target = (ImGui::GetScrollY() - (clip_rect.Min.y - selected_bb.Min.y));
        else if (selected_bb.Max.y > clip_rect.Max.y)
          scroll_target = (ImGui::GetScrollY() + (selected_bb.Max.y - clip_rect.Max.y));
        scroll_target = std::clamp(scroll_target, 0.0f, window->ScrollMax.y);

        if (scroll_target != s_state.scroll_animated.GetEndValue())
          s_state.scroll_animated.Start(ImGui::GetScrollY(), scroll_target, SCROLL_ANIMATION_TIME);
      }
      if (s_state.scroll_animated.IsActive())
        ImGui::SetScrollY(s_state.scroll_animated.UpdateAndGetValue());

      if (s_state.background_animated.GetEndValue() != selected_y)
        s_state.background_animated.Start(s_state.background_animated.UpdateAndGetValue(), selected_y,
                                          BG_ANIMATION_TIME);
      FullscreenUI::BeginScrollFade();
      const float highlight_y =
        origin.y +
        (s_state.background_animated.IsActive() ? s_state.background_animated.UpdateAndGetValue() : selected_y);
      list_dl->AddRectFilled(ImVec2(origin.x, highlight_y), ImVec2(origin.x + row_width, highlight_y + row_height),
                             ImGui::GetColorU32(UIStyle.PopupHighlight), row_rounding);
      list_dl->AddRect(ImVec2(origin.x, highlight_y), ImVec2(origin.x + row_width, highlight_y + row_height),
                       accent_color, row_rounding, LayoutScale(2.0f));

      const float text_height = UIStyle.MediumFontSize + spacing + UIStyle.MediumSmallFontSize;
      for (size_t i = 0; i < s_state.slots.size(); i++)
      {
        const ListEntry& entry = s_state.slots[i];
        const ImVec2 row_min(origin.x, origin.y + row_height * static_cast<float>(i));
        if (row_min.y > clip_rect.Max.y || (row_min.y + row_height) < clip_rect.Min.y)
          continue;

        const ImRect thumb_bb(row_min + ImVec2(row_padding, row_padding),
                              row_min + ImVec2(row_padding, row_padding) + thumb_size);
        draw_thumbnail(list_dl, entry, thumb_bb, LayoutScale(6.0f), true, 1.0f);

        const bool selected = (i == selected_index);
        const ImVec2 text_min(thumb_bb.Max.x + row_padding * 1.5f, row_min.y + (row_height - text_height) * 0.5f);
        const ImVec2 text_max(row_min.x + row_width - row_padding, row_min.y + row_height);
        RenderShadowedTextClipped(list_dl, font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_min, text_max,
                                  selected ? ImGui::GetColorU32(UIStyle.PrimaryTextColor) : text_color,
                                  slot_title(entry));
        RenderShadowedTextClipped(list_dl, font, UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight,
                                  ImVec2(text_min.x, text_min.y + UIStyle.MediumFontSize + spacing), text_max,
                                  muted_color, entry.summary);
      }

      FullscreenUI::EndScrollFade();
      ImGui::ItemSize(ImVec2(row_width, row_height * static_cast<float>(s_state.slots.size())));
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(1);

  // auto-close
  if (!s_state.is_closing)
  {
    s_state.open_time += io.DeltaTime;
    if (s_state.open_time >= s_state.close_time)
    {
      Close();
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
      // Need to cancel the hotkey bindings, otherwise the pause menu will open.
      // This is a bit janky because if the key is released before the core thread picks it up,
      // it'll be too late. Chances of that are pretty slim.
      Host::RunOnCoreThread([]() { InputManager::ClearBindStateFromSource(InputManager::MakeHostKeyboardKey(0)); });
      Close();
    }
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

void SaveStateSelectorUI::LoadCurrentSlot()
{
  DebugAssert(VideoThread::IsOnThread());

  Host::RunOnCoreThread(
    [global = IsCurrentSlotGlobal(), slot = GetCurrentSlot()]() { System::LoadStateFromSlot(global, slot); });

  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  Host::RunOnCoreThread(
    [global = IsCurrentSlotGlobal(), slot = GetCurrentSlot()]() { System::SaveStateToSlot(global, slot); });

  Close();
}

void SaveStateSelectorUI::ShowSlotOSDMessage()
{
  const std::string path = IsCurrentSlotGlobal() ?
                             System::GetGlobalSaveStatePath(GetCurrentSlot()) :
                             System::GetGameSaveStatePath(VideoThread::GetGameSerial(), GetCurrentSlot());
  FILESYSTEM_STAT_DATA sd;
  std::string date;
  if (!path.empty() && FileSystem::StatFile(path.c_str(), &sd))
  {
    date = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Saved {}"),
                       Host::FormatRelativeDateTime(sd.ModificationTime, false, false));
  }
  else
  {
    date = TRANSLATE_STR("SaveStateSelectorUI", "No save in this slot.");
  }

  Host::AddIconOSDMessage(
    OSDMessageType::Quick, "ShowSlotOSDMessage", ICON_EMOJI_MAGNIFIYING_GLASS_TILTED_LEFT,
    IsCurrentSlotGlobal() ?
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Global Save Slot {} selected."), GetCurrentSlot()) :
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Save Slot {0} selected."), GetCurrentSlot()),
    std::move(date));
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
