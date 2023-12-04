// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "imgui_overlays.h"
#include "cdrom.h"
#include "controller.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "host.h"
#include "mdec.h"
#include "resources.h"
#include "settings.h"
#include "spu.h"
#include "system.h"

#include "util/audio_stream.h"
#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsFontAwesome5.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
static void FormatProcessorStat(SmallStringBase& text, double usage, double time);
static void DrawPerformanceOverlay();
static void DrawEnhancementsOverlay();
static void DrawInputsOverlay();
} // namespace ImGuiManager

namespace SaveStateSelectorUI {
static void Draw();
}

static std::tuple<float, float> GetMinMax(std::span<const float> values)
{
#if defined(CPU_ARCH_SSE)
  __m128 vmin(_mm_loadu_ps(values.data()));
  __m128 vmax(vmin);

  const u32 count = static_cast<u32>(values.size());
  const u32 aligned_count = Common::AlignDownPow2(count, 4);
  u32 i = 4;
  for (; i < aligned_count; i += 4)
  {
    const __m128 v(_mm_loadu_ps(&values[i]));
    vmin = _mm_min_ps(vmin, v);
    vmax = _mm_max_ps(vmax, v);
  }

#if defined(_MSC_VER) && !defined(__clang__)
  float min = std::min(vmin.m128_f32[0], std::min(vmin.m128_f32[1], std::min(vmin.m128_f32[2], vmin.m128_f32[3])));
  float max = std::max(vmax.m128_f32[0], std::max(vmax.m128_f32[1], std::max(vmax.m128_f32[2], vmax.m128_f32[3])));
#else
  float min = std::min(vmin[0], std::min(vmin[1], std::min(vmin[2], vmin[3])));
  float max = std::max(vmax[0], std::max(vmax[1], std::max(vmax[2], vmax[3])));
#endif
  for (; i < count; i++)
  {
    min = std::min(min, values[i]);
    max = std::max(max, values[i]);
  }

  return std::tie(min, max);
#elif defined(CPU_ARCH_NEON)
  float32x4_t vmin(vld1q_f32(values.data()));
  float32x4_t vmax(vmin);

  const u32 count = static_cast<u32>(values.size());
  const u32 aligned_count = Common::AlignDownPow2(count, 4);
  u32 i = 4;
  for (; i < aligned_count; i += 4)
  {
    const float32x4_t v(vld1q_f32(&values[i]));
    vmin = vminq_f32(vmin, v);
    vmax = vmaxq_f32(vmax, v);
  }

  float min = vminvq_f32(vmin);
  float max = vmaxvq_f32(vmax);
  for (; i < count; i++)
  {
    min = std::min(min, values[i]);
    max = std::max(max, values[i]);
  }

  return std::tie(min, max);
#else
  float min = values[0];
  float max = values[0];
  const u32 count = static_cast<u32>(values.size());
  for (u32 i = 1; i < count; i++)
  {
    min = std::min(min, values[i]);
    max = std::max(max, values[i]);
  }

  return std::tie(min, max);
#endif
}

static bool s_save_state_selector_ui_open = false;

void Host::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/, int progress_max /*= -1*/,
                                int progress_value /*= -1*/)
{
  const auto& io = ImGui::GetIO();
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = (400.0f * scale);
  const bool has_progress = (progress_min < progress_max);

  // eat the last imgui frame, it might've been partially rendered by the caller.
  ImGui::EndFrame();
  ImGui::NewFrame();

  const float logo_width = 260.0f * scale;
  const float logo_height = 260.0f * scale;

  ImGui::SetNextWindowSize(ImVec2(logo_width, logo_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) - (50.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("LoadingScreenLogo", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground))
  {
    GPUTexture* tex = ImGuiFullscreen::GetCachedTexture("images/duck.png");
    if (tex)
      ImGui::Image(tex, ImVec2(logo_width, logo_height));
  }
  ImGui::End();

  const float padding_and_rounding = 15.0f * scale;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
  ImGui::SetNextWindowSize(ImVec2(width, (has_progress ? 80.0f : 50.0f) * scale), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) + (100.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  if (ImGui::Begin("LoadingScreen", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (has_progress)
    {
      ImGui::TextUnformatted(message);

      TinyString buf;
      buf.fmt("{}/{}", progress_value, progress_max);

      const ImVec2 prog_size = ImGui::CalcTextSize(buf.c_str(), buf.end_ptr());
      ImGui::SameLine();
      ImGui::SetCursorPosX(width - padding_and_rounding - prog_size.x);
      ImGui::TextUnformatted(buf.c_str(), buf.end_ptr());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);

      ImGui::ProgressBar(static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min),
                         ImVec2(-1.0f, 0.0f), "");
      Log_InfoPrintf("%s: %d/%d", message, progress_value, progress_max);
    }
    else
    {
      const ImVec2 text_size(ImGui::CalcTextSize(message));
      ImGui::SetCursorPosX((width - text_size.x) / 2.0f);
      ImGui::TextUnformatted(message);
      Log_InfoPrintf("%s", message);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  ImGui::EndFrame();

  // TODO: Glass effect or something.

  if (g_gpu_device->BeginPresent(false))
  {
    g_gpu_device->RenderImGui();
    g_gpu_device->EndPresent();
  }

  ImGui::NewFrame();
}

void ImGuiManager::RenderDebugWindows()
{
  if (System::IsValid())
  {
    if (g_settings.debugging.show_gpu_state)
      g_gpu->DrawDebugStateWindow();
    if (g_settings.debugging.show_cdrom_state)
      CDROM::DrawDebugWindow();
    if (g_settings.debugging.show_timers_state)
      Timers::DrawDebugStateWindow();
    if (g_settings.debugging.show_spu_state)
      SPU::DrawDebugStateWindow();
    if (g_settings.debugging.show_mdec_state)
      MDEC::DrawDebugStateWindow();
    if (g_settings.debugging.show_dma_state)
      DMA::DrawDebugStateWindow();
  }
}

void ImGuiManager::RenderTextOverlays()
{
  const System::State state = System::GetState();
  if (state != System::State::Shutdown)
  {
    DrawPerformanceOverlay();

    if (g_settings.display_show_enhancements && state != System::State::Paused)
      DrawEnhancementsOverlay();

    if (g_settings.display_show_inputs && state != System::State::Paused)
      DrawInputsOverlay();
  }
}

void ImGuiManager::RenderOverlayWindows()
{
  const System::State state = System::GetState();
  if (state != System::State::Shutdown)
  {
    if (s_save_state_selector_ui_open)
      SaveStateSelectorUI::Draw();
  }
}

void ImGuiManager::FormatProcessorStat(SmallStringBase& text, double usage, double time)
{
  // Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
  // which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
  // so that people don't get confused, and remove the decimal places when it's there while we're at it.
  if (usage >= 99.95)
    text.append_fmt("100% ({:.2f}ms)", time);
  else
    text.append_fmt("{:.1f}% ({:.2f}ms)", usage, time);
}

void ImGuiManager::DrawPerformanceOverlay()
{
  if (!(g_settings.display_show_fps || g_settings.display_show_speed || g_settings.display_show_resolution ||
        g_settings.display_show_cpu ||
        (g_settings.display_show_status_indicators &&
         (System::IsPaused() || System::IsFastForwardEnabled() || System::IsTurboEnabled()))))
  {
    return;
  }

  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = std::ceil(1.0f * scale);
  const float margin = std::ceil(10.0f * scale);
  const float spacing = std::ceil(5.0f * scale);
  ImFont* fixed_font = ImGuiManager::GetFixedFont();
  ImFont* standard_font = ImGuiManager::GetStandardFont();
  float position_y = margin;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  SmallString text;
  ImVec2 text_size;
  bool first = true;

#define DRAW_LINE(font, text, color)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    text_size =                                                                                                        \
      font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr);         \
    dl->AddText(                                                                                                       \
      font, font->FontSize,                                                                                            \
      ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),         \
      IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());                                                           \
    dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y), color,  \
                (text));                                                                                               \
    position_y += text_size.y + spacing;                                                                               \
  } while (0)

  const System::State state = System::GetState();
  if (state == System::State::Running)
  {
    const float speed = System::GetEmulationSpeed();
    if (g_settings.display_show_fps)
    {
      text.append_fmt("G: {:.2f} | V: {:.2f}", System::GetFPS(), System::GetVPS());
      first = false;
    }
    if (g_settings.display_show_speed)
    {
      text.append_fmt("{}{}%", first ? "" : " | ", static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.append(" (Max)");
      else
        text.append_fmt(" ({:.0f}%)", target_speed * 100.0f);

      first = false;
    }
    if (!text.empty())
    {
      ImU32 color;
      if (speed < 95.0f)
        color = IM_COL32(255, 100, 100, 255);
      else if (speed > 105.0f)
        color = IM_COL32(100, 255, 100, 255);
      else
        color = IM_COL32(255, 255, 255, 255);

      DRAW_LINE(fixed_font, text, color);
    }

    if (g_settings.display_show_resolution)
    {
      // TODO: this seems wrong?
      const auto [effective_width, effective_height] = g_gpu->GetEffectiveDisplayResolution();
      const bool interlaced = g_gpu->IsInterlacedDisplayEnabled();
      const bool pal = g_gpu->IsInPALMode();
      text.fmt("{}x{} {} {}", effective_width, effective_height, pal ? "PAL" : "NTSC",
               interlaced ? "Interlaced" : "Progressive");
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_cpu)
    {
      text.fmt("{:.2f}ms | {:.2f}ms | {:.2f}ms", System::GetMinimumFrameTime(), System::GetAverageFrameTime(),
               System::GetMaximumFrameTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (g_settings.cpu_overclock_active ||
          (g_settings.cpu_execution_mode != CPUExecutionMode::Recompiler || g_settings.cpu_recompiler_icache ||
           g_settings.cpu_recompiler_memory_exceptions))
      {
        first = true;
        text.assign("CPU[");
        if (g_settings.cpu_overclock_active)
        {
          text.append_fmt("{}", g_settings.GetCPUOverclockPercent());
          first = false;
        }
        if (g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter)
        {
          text.append_fmt("{}{}", first ? "" : "/", "I");
          first = false;
        }
        else if (g_settings.cpu_execution_mode == CPUExecutionMode::CachedInterpreter)
        {
          text.append_fmt("{}{}", first ? "" : "/", "CI");
          first = false;
        }
        else if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
        {
          text.append_fmt("{}{}", first ? "" : "/", "NR");
          first = false;
        }
        else
        {
          if (g_settings.cpu_recompiler_icache)
          {
            text.append_fmt("{}{}", first ? "" : "/", "IC");
            first = false;
          }
          if (g_settings.cpu_recompiler_memory_exceptions)
          {
            text.append_fmt("{}{}", first ? "" : "/", "ME");
            first = false;
          }
        }

        text.append("]: ");
      }
      else
      {
        text.assign("CPU: ");
      }
      FormatProcessorStat(text, System::GetCPUThreadUsage(), System::GetCPUThreadAverageTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (g_gpu->GetSWThread())
      {
        text.assign("SW: ");
        FormatProcessorStat(text, System::GetSWThreadUsage(), System::GetSWThreadAverageTime());
        DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
      }

#if 0
      {
        AudioStream* stream = g_spu.GetOutputStream();
        const u32 frames = stream->GetBufferedFramesRelaxed();
        text.fmt("Audio: {:<4u}f/{:<3u}ms", frames, AudioStream::GetMSForBufferSize(stream->GetSampleRate(), frames));
        DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
      }
#endif
    }

    if (g_settings.display_show_gpu && g_gpu_device->IsGPUTimingEnabled())
    {
      text.assign("GPU: ");
      FormatProcessorStat(text, System::GetGPUUsage(), System::GetGPUAverageTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_status_indicators)
    {
      const bool rewinding = System::IsRewinding();
      if (rewinding || System::IsFastForwardEnabled() || System::IsTurboEnabled())
      {
        text.assign(rewinding ? ICON_FA_FAST_BACKWARD : ICON_FA_FAST_FORWARD);
        DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
      }
    }

    if (g_settings.display_show_frame_times)
    {
      const ImVec2 history_size(200.0f * scale, 50.0f * scale);
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
      if (ImGui::Begin("##frame_times", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs))
      {
        ImGui::PushFont(fixed_font);

        auto [min, max] = GetMinMax(System::GetFrameTimeHistory());

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
            return System::GetFrameTimeHistory()[((System::GetFrameTimeHistoryPos() + idx) %
                                                  System::NUM_FRAME_TIME_SAMPLES)];
          },
          nullptr, System::NUM_FRAME_TIME_SAMPLES, 0, nullptr, min, max, history_size);

        ImDrawList* win_dl = ImGui::GetCurrentWindow()->DrawList;
        const ImVec2 wpos(ImGui::GetCurrentWindow()->Pos);

        text.fmt("{:.1f} ms", max);
        text_size = fixed_font->CalcTextSizeA(fixed_font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
        win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset, wpos.y + shadow_offset),
                        IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
        win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y), IM_COL32(255, 255, 255, 255),
                        text.c_str(), text.end_ptr());

        text.fmt("{:.1f} ms", min);
        text_size = fixed_font->CalcTextSizeA(fixed_font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
        win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset,
                               wpos.y + history_size.y - fixed_font->FontSize + shadow_offset),
                        IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
        win_dl->AddText(
          ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y + history_size.y - fixed_font->FontSize),
          IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
        ImGui::PopFont();
      }
      ImGui::End();
      ImGui::PopStyleVar(5);
      ImGui::PopStyleColor(3);
    }
  }
  else if (g_settings.display_show_status_indicators && state == System::State::Paused &&
           !FullscreenUI::HasActiveWindow())
  {
    text.assign(ICON_FA_PAUSE);
    DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
  }

#undef DRAW_LINE
}

void ImGuiManager::DrawEnhancementsOverlay()
{
  LargeString text;
  text.append_fmt("{} {}-{}", Settings::GetConsoleRegionName(System::GetRegion()),
                  GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()),
                  g_gpu->IsHardwareRenderer() ? "HW" : "SW");

  if (g_settings.rewind_enable)
    text.append_fmt(" RW={}/{}", g_settings.rewind_save_frequency, g_settings.rewind_save_slots);
  if (g_settings.IsRunaheadEnabled())
    text.append_fmt(" RA={}", g_settings.runahead_frames);

  if (g_settings.cpu_overclock_active)
    text.append_fmt(" CPU={}%", g_settings.GetCPUOverclockPercent());
  if (g_settings.enable_8mb_ram)
    text.append(" 8MB");
  if (g_settings.cdrom_read_speedup != 1)
    text.append_fmt(" CDR={}x", g_settings.cdrom_read_speedup);
  if (g_settings.cdrom_seek_speedup != 1)
    text.append_fmt(" CDS={}x", g_settings.cdrom_seek_speedup);
  if (g_settings.gpu_resolution_scale != 1)
    text.append_fmt(" IR={}x", g_settings.gpu_resolution_scale);
  if (g_settings.gpu_multisamples != 1)
  {
    text.append_fmt(" {}x{}", g_settings.gpu_multisamples, g_settings.gpu_per_sample_shading ? "SSAA" : "MSAA");
  }
  if (g_settings.gpu_true_color)
    text.append(" TrueCol");
  if (g_settings.gpu_disable_interlacing)
    text.append(" ForceProg");
  if (g_settings.gpu_force_ntsc_timings && System::GetRegion() == ConsoleRegion::PAL)
    text.append(" PAL60");
  if (g_settings.gpu_texture_filter != GPUTextureFilter::Nearest)
    text.append_fmt(" {}", Settings::GetTextureFilterName(g_settings.gpu_texture_filter));
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto &&
      g_settings.display_aspect_ratio != DisplayAspectRatio::R4_3)
  {
    text.append(" WSHack");
  }
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
  const float margin = 10.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - font->FontSize;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImVec2 text_size = font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, text.c_str(),
                                         text.end_ptr(), nullptr);
  dl->AddText(font, font->FontSize,
              ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),
              IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
  dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
              IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
}

void ImGuiManager::DrawInputsOverlay()
{
  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  const float spacing = 5.0f * scale;
  ImFont* font = ImGuiManager::GetStandardFont();

  static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);
  static constexpr u32 shadow_color = IM_COL32(0x00, 0x00, 0x00, 100);

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  u32 num_ports = 0;
  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] != ControllerType::None)
      num_ports++;
  }

  float current_x = margin;
  float current_y = display_size.y - margin - ((static_cast<float>(num_ports) * (font->FontSize + spacing)) - spacing);

  const ImVec4 clip_rect(current_x, current_y, display_size.x - margin, display_size.y - margin);

  SmallString text;

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] == ControllerType::None)
      continue;

    const Controller* controller = System::GetController(port);
    const Controller::ControllerInfo* cinfo =
      controller ? Controller::GetControllerInfo(controller->GetType()) : nullptr;
    if (!cinfo)
      continue;

    if (cinfo->icon_name)
      text.append_fmt("{} {}", cinfo->icon_name, port + 1u);
    else
      text.append_fmt("{} |", port + 1u);

    for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
    {
      switch (bi.type)
      {
        case InputBindingInfo::Type::Axis:
        case InputBindingInfo::Type::HalfAxis:
        {
          // axes are always shown
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= (254.0f / 255.0f))
            text.append_fmt(" {}", bi.icon_name ? bi.icon_name : bi.name);
          else if (value > (1.0f / 255.0f))
            text.append_fmt(" {}: {:.2f}", bi.icon_name ? bi.icon_name : bi.name, value);
        }
        break;

        case InputBindingInfo::Type::Button:
        {
          // buttons only shown when active
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= 0.5f)
            text.append_fmt(" {}", bi.icon_name ? bi.icon_name : bi.name);
        }
        break;

        case InputBindingInfo::Type::Motor:
        case InputBindingInfo::Type::Macro:
        case InputBindingInfo::Type::Unknown:
        case InputBindingInfo::Type::Pointer:
        default:
          break;
      }
    }

    dl->AddText(font, font->FontSize, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
                text.c_str(), text.end_ptr(), 0.0f, &clip_rect);
    dl->AddText(font, font->FontSize, ImVec2(current_x, current_y), text_color, text.c_str(), text.end_ptr(), 0.0f,
                &clip_rect);

    current_y += font->FontSize + spacing;
  }
}

namespace SaveStateSelectorUI {
struct ListEntry
{
  std::string path;
  std::string serial;
  std::string title;
  std::string formatted_timestamp;
  std::unique_ptr<GPUTexture> preview_texture;
  s32 slot;
  bool global;
};

static void InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot, bool global);
static void InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, std::string path, s32 slot, bool global);

static void RefreshHotkeyLegend();

static std::string s_load_legend;
static std::string s_save_legend;
static std::string s_prev_legend;
static std::string s_next_legend;

static std::vector<ListEntry> s_slots;
static u32 s_current_selection = 0;

static Common::Timer s_open_timer;
static float s_open_time = 0.0f;
} // namespace SaveStateSelectorUI

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
  s_open_timer.Reset();
  s_open_time = open_time;

  if (s_save_state_selector_ui_open)
    return;

  s_save_state_selector_ui_open = true;
  RefreshList();
  RefreshHotkeyLegend();
}

void SaveStateSelectorUI::Close(bool reset_slot)
{
  s_save_state_selector_ui_open = false;
  s_load_legend = {};
  s_save_legend = {};
  s_prev_legend = {};
  s_next_legend = {};
  if (reset_slot)
    s_current_selection = 0;
}

void SaveStateSelectorUI::RefreshList()
{
  for (ListEntry& entry : s_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
  s_slots.clear();

  if (System::IsShutdown())
    return;

  if (!System::GetGameSerial().empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::string path(System::GetGameSaveStateFileName(System::GetGameSerial(), i));
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, false);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, false);

      s_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::string path(System::GetGlobalSaveStateFileName(i));
    std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

    ListEntry li;
    if (ssi)
      InitializeListEntry(&li, &ssi.value(), std::move(path), i, true);
    else
      InitializePlaceholderListEntry(&li, std::move(path), i, true);

    s_slots.push_back(std::move(li));
  }

  if (s_slots.empty() || s_current_selection >= s_slots.size())
    s_current_selection = 0;
}

void SaveStateSelectorUI::DestroyTextures()
{
  Close();

  for (ListEntry& entry : s_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  auto format_legend_entry = [](std::string_view setting, std::string_view caption) {
    auto slash_pos = setting.find_first_of('/');
    if (slash_pos != setting.npos)
    {
      setting = setting.substr(slash_pos + 1);
    }

    return fmt::format("{} - {}", setting, caption);
  };

  s_load_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "LoadSelectedSaveState"),
                                      TRANSLATE_STR("SaveStateSelectorUI", "Load"));
  s_save_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SaveSelectedSaveState"),
                                      TRANSLATE_STR("SaveStateSelectorUI", "Save"));
  s_prev_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SelectPreviousSaveStateSlot"),
                                      TRANSLATE_STR("SaveStateSelectorUI", "Select Previous"));
  s_next_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SelectNextSaveStateSlot"),
                                      TRANSLATE_STR("SaveStateSelectorUI", "Select Next"));
}

void SaveStateSelectorUI::SelectNextSlot()
{
  if (!s_save_state_selector_ui_open)
    Open();

  s_open_timer.Reset();
  s_current_selection = (s_current_selection == static_cast<u32>(s_slots.size() - 1)) ? 0 : (s_current_selection + 1);
}

void SaveStateSelectorUI::SelectPreviousSlot()
{
  if (!s_save_state_selector_ui_open)
    Open();

  s_open_timer.Reset();
  s_current_selection =
    (s_current_selection == 0) ? (static_cast<u32>(s_slots.size()) - 1u) : (s_current_selection - 1);
}

void SaveStateSelectorUI::InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, std::string path, s32 slot,
                                              bool global)
{
  li->title = std::move(ssi->title);
  li->serial = std::move(ssi->serial);
  li->path = std::move(path);
  li->formatted_timestamp = fmt::format("{:%c}", fmt::localtime(ssi->timestamp));
  li->slot = slot;
  li->global = global;

  // Might not have a display yet, we're called at startup..
  if (g_gpu_device)
  {
    g_gpu_device->RecycleTexture(std::move(li->preview_texture));

    if (ssi && !ssi->screenshot_data.empty())
    {
      li->preview_texture = g_gpu_device->FetchTexture(
        ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
        ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width);
    }
    else
    {
      li->preview_texture = g_gpu_device->FetchTexture(
        Resources::PLACEHOLDER_ICON_WIDTH, Resources::PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, GPUTexture::Type::Texture,
        GPUTexture::Format::RGBA8, Resources::PLACEHOLDER_ICON_DATA, sizeof(u32) * Resources::PLACEHOLDER_ICON_WIDTH);
    }

    if (!li->preview_texture)
      Log_ErrorPrintf("Failed to upload save state image to GPU");
  }
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot, bool global)
{
  li->title = TRANSLATE_STR("SaveStateSelectorUI", "No Save State");
  std::string().swap(li->serial);
  li->path = std::move(path);
  std::string().swap(li->formatted_timestamp);
  li->slot = slot;
  li->global = global;

  if (g_gpu_device)
  {
    g_gpu_device->RecycleTexture(std::move(li->preview_texture));

    li->preview_texture = g_gpu_device->FetchTexture(
      Resources::PLACEHOLDER_ICON_WIDTH, Resources::PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, GPUTexture::Type::Texture,
      GPUTexture::Format::RGBA8, Resources::PLACEHOLDER_ICON_DATA, sizeof(u32) * Resources::PLACEHOLDER_ICON_WIDTH);
    if (!li->preview_texture)
      Log_ErrorPrintf("Failed to upload save state image to GPU");
  }
}

void SaveStateSelectorUI::Draw()
{
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const float window_width = ImGui::GetIO().DisplaySize.x * (2.0f / 3.0f);
  const float window_height = ImGui::GetIO().DisplaySize.y * 0.5f;
  const float rounding = 4.0f * framebuffer_scale;
  ImGui::SetNextWindowSize(ImVec2(window_width, window_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.15f, 0.17f, 0.8f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);

  if (ImGui::Begin("##save_state_selector", nullptr,
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar))
  {
    // Leave 2 lines for the legend
    const float legend_margin = ImGui::GetFontSize() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float padding = 10.0f * framebuffer_scale;

    ImGui::BeginChild("##item_list", ImVec2(0, -legend_margin), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar);
    {
      const ImVec2 image_size = ImVec2(128.0f * framebuffer_scale, (128.0f / (4.0f / 3.0f)) * framebuffer_scale);
      const float item_height = image_size.y + padding * 2.0f;
      const float text_indent = image_size.x + padding + padding;

      for (size_t i = 0; i < s_slots.size(); i++)
      {
        const ListEntry& entry = s_slots[i];
        const float y_start = item_height * static_cast<float>(i);

        if (i == s_current_selection)
        {
          ImGui::SetCursorPosY(y_start);
          ImGui::SetScrollHereY();

          const ImVec2 p_start(ImGui::GetCursorScreenPos());
          const ImVec2 p_end(p_start.x + window_width, p_start.y + item_height);
          ImGui::GetWindowDrawList()->AddRectFilled(p_start, p_end, ImColor(0.22f, 0.30f, 0.34f, 0.9f), rounding);
        }

        if (entry.preview_texture)
        {
          ImGui::SetCursorPosY(y_start + padding);
          ImGui::SetCursorPosX(padding);
          ImGui::Image(entry.preview_texture.get(), image_size);
        }

        ImGui::SetCursorPosY(y_start + padding);

        ImGui::Indent(text_indent);

        if (entry.global)
        {
          ImGui::Text(TRANSLATE("SaveStateSelectorUI", "Global Slot %d"), entry.slot);
        }
        else if (entry.serial.empty())
        {
          ImGui::Text(TRANSLATE("SaveStateSelectorUI", "Game Slot %d"), entry.slot);
        }
        else
        {
          ImGui::Text(TRANSLATE("SaveStateSelectorUI", "%s Slot %d"), entry.serial.c_str(), entry.slot);
        }
        ImGui::TextUnformatted(entry.title.c_str());
        ImGui::TextUnformatted(entry.formatted_timestamp.c_str());
        ImGui::TextUnformatted(entry.path.c_str());

        ImGui::Unindent(text_indent);
      }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##legend", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar);
    {
      ImGui::SetCursorPosX(padding);
      ImGui::BeginTable("table", 2);

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s_load_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s_prev_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s_save_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s_next_legend.c_str());

      ImGui::EndTable();
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  // auto-close
  if (s_open_timer.GetTimeSeconds() >= s_open_time)
    Close();
}

void SaveStateSelectorUI::LoadCurrentSlot()
{
  if (s_slots.empty() || s_current_selection >= s_slots.size() || s_slots[s_current_selection].path.empty())
    return;

  System::LoadState(s_slots[s_current_selection].path.c_str());
  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  if (s_slots.empty() || s_current_selection >= s_slots.size() || s_slots[s_current_selection].path.empty())
    return;

  System::SaveState(s_slots[s_current_selection].path.c_str(), g_settings.create_save_state_backups);
  Close();
}
