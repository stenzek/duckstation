#include "imgui_overlays.h"
#include "IconsFontAwesome5.h"
#include "achievements.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "common_host.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/settings.h"
#include "core/system.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "fullscreen_ui.h"
#include "icon.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_internal.h"
#include "imgui_manager.h"
#include "input_manager.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
static void FormatProcessorStat(String& text, double usage, double time);
static void DrawPerformanceOverlay();
static void DrawEnhancementsOverlay();
static void DrawInputsOverlay();
} // namespace ImGuiManager

namespace SaveStateSelectorUI {
static void Draw();
}

static bool s_save_state_selector_ui_open = false;

void ImGuiManager::RenderOverlays()
{
  if (System::IsValid())
  {
    DrawPerformanceOverlay();

    if (g_settings.display_show_enhancements)
      DrawEnhancementsOverlay();

    if (g_settings.display_show_inputs)
      DrawInputsOverlay();

    if (s_save_state_selector_ui_open)
      SaveStateSelectorUI::Draw();
  }
}

void ImGuiManager::FormatProcessorStat(String& text, double usage, double time)
{
  // Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
  // which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
  // so that people don't get confused, and remove the decimal places when it's there while we're at it.
  if (usage >= 99.95)
    text.AppendFmtString("100% ({:.2f}ms)", time);
  else
    text.AppendFmtString("{:.1f}% ({:.2f}ms)", usage, time);
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
      IM_COL32(0, 0, 0, 100), text, text.GetCharArray() + text.GetLength());                                           \
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
      text.AppendFmtString("G: {:.2f} | V: {:.2f}", System::GetFPS(), System::GetVPS());
      first = false;
    }
    if (g_settings.display_show_speed)
    {
      text.AppendFmtString("{}{}%", first ? "" : " | ", static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.AppendString(" (Max)");
      else
        text.AppendFmtString(" ({:.0f}%)", target_speed * 100.0f);

      first = false;
    }
    if (!text.IsEmpty())
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
      const auto [effective_width, effective_height] = g_gpu->GetEffectiveDisplayResolution();
      const bool interlaced = g_gpu->IsInterlacedDisplayEnabled();
      text.Fmt("{}x{} ({})", effective_width, effective_height, interlaced ? "interlaced" : "progressive");
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_cpu)
    {
      text.Clear();
      text.AppendFmtString("{:.2f}ms ({:.2f}ms worst)", System::GetAverageFrameTime(), System::GetWorstFrameTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      text.Clear();
      if (g_settings.cpu_overclock_active)
        text.Fmt("CPU[{}]: ", g_settings.GetCPUOverclockPercent());
      else
        text.Assign("CPU: ");
      FormatProcessorStat(text, System::GetCPUThreadUsage(), System::GetCPUThreadAverageTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (!g_gpu->IsHardwareRenderer() && g_settings.gpu_use_thread)
      {
        text.Assign("SW: ");
        FormatProcessorStat(text, System::GetSWThreadUsage(), System::GetSWThreadAverageTime());
        DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
      }
    }

    if (g_settings.display_show_status_indicators)
    {
      const bool rewinding = System::IsRewinding();
      if (rewinding || System::IsFastForwardEnabled() || System::IsTurboEnabled())
      {
        text.Assign(rewinding ? ICON_FA_FAST_BACKWARD : ICON_FA_FAST_FORWARD);
        DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
      }
    }
  }
  else if (g_settings.display_show_status_indicators && state == System::State::Paused)
  {
    text.Assign(ICON_FA_PAUSE);
    DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
  }

#undef DRAW_LINE
}

void ImGuiManager::DrawEnhancementsOverlay()
{
  LargeString text;
  text.AppendString(Settings::GetConsoleRegionName(System::GetRegion()));

  if (g_settings.rewind_enable)
    text.AppendFormattedString(" RW=%g/%u", g_settings.rewind_save_frequency, g_settings.rewind_save_slots);
  if (g_settings.IsRunaheadEnabled())
    text.AppendFormattedString(" RA=%u", g_settings.runahead_frames);

  if (g_settings.cpu_overclock_active)
    text.AppendFormattedString(" CPU=%u%%", g_settings.GetCPUOverclockPercent());
  if (g_settings.enable_8mb_ram)
    text.AppendString(" 8MB");
  if (g_settings.cdrom_read_speedup != 1)
    text.AppendFormattedString(" CDR=%ux", g_settings.cdrom_read_speedup);
  if (g_settings.cdrom_seek_speedup != 1)
    text.AppendFormattedString(" CDS=%ux", g_settings.cdrom_seek_speedup);
  if (g_settings.gpu_resolution_scale != 1)
    text.AppendFormattedString(" IR=%ux", g_settings.gpu_resolution_scale);
  if (g_settings.gpu_multisamples != 1)
  {
    text.AppendFormattedString(" %ux%s", g_settings.gpu_multisamples,
                               g_settings.gpu_per_sample_shading ? "MSAA" : "SSAA");
  }
  if (g_settings.gpu_true_color)
    text.AppendString(" TrueCol");
  if (g_settings.gpu_disable_interlacing)
    text.AppendString(" ForceProg");
  if (g_settings.gpu_force_ntsc_timings && System::GetRegion() == ConsoleRegion::PAL)
    text.AppendString(" PAL60");
  if (g_settings.gpu_texture_filter != GPUTextureFilter::Nearest)
    text.AppendFormattedString(" %s", Settings::GetTextureFilterName(g_settings.gpu_texture_filter));
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto &&
      g_settings.display_aspect_ratio != DisplayAspectRatio::R4_3)
  {
    text.AppendString(" WSHack");
  }
  if (g_settings.gpu_pgxp_enable)
  {
    text.AppendString(" PGXP");
    if (g_settings.gpu_pgxp_culling)
      text.AppendString("/Cull");
    if (g_settings.gpu_pgxp_texture_correction)
      text.AppendString("/Tex");
    if (g_settings.gpu_pgxp_vertex_cache)
      text.AppendString("/VC");
    if (g_settings.gpu_pgxp_cpu)
      text.AppendString("/CPU");
    if (g_settings.gpu_pgxp_depth_buffer)
      text.AppendString("/Depth");
  }

  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - font->FontSize;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImVec2 text_size = font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, text,
                                         text.GetCharArray() + text.GetLength(), nullptr);
  dl->AddText(font, font->FontSize,
              ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),
              IM_COL32(0, 0, 0, 100), text, text.GetCharArray() + text.GetLength());
  dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
              IM_COL32(255, 255, 255, 255), text, text.GetCharArray() + text.GetLength());
}

void ImGuiManager::DrawInputsOverlay()
{
  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  const float spacing = 5.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();

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

  LargeString text;

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] == ControllerType::None)
      continue;

    const Controller* controller = System::GetController(port);
    const Controller::ControllerInfo* cinfo =
      controller ? Controller::GetControllerInfo(controller->GetType()) : nullptr;
    if (!cinfo)
      continue;

    text.Fmt("P{} |", port + 1u);

    for (u32 bind = 0; bind < cinfo->num_bindings; bind++)
    {
      const Controller::ControllerBindingInfo& bi = cinfo->bindings[bind];
      switch (bi.type)
      {
        case Controller::ControllerBindingType::Axis:
        case Controller::ControllerBindingType::HalfAxis:
        {
          // axes are always shown
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= (254.0f / 255.0f))
            text.AppendFmtString(" {}", bi.name);
          else if (value > (1.0f / 255.0f))
            text.AppendFmtString(" {}: {:.2f}", bi.name, value);
        }
        break;

        case Controller::ControllerBindingType::Button:
        {
          // buttons only shown when active
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= 0.5f)
            text.AppendFmtString(" {}", bi.name);
        }
        break;

        case Controller::ControllerBindingType::Motor:
        case Controller::ControllerBindingType::Macro:
        case Controller::ControllerBindingType::Unknown:
        default:
          break;
      }
    }

    dl->AddText(font, font->FontSize, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
                text.GetCharArray(), text.GetCharArray() + text.GetLength(), 0.0f, &clip_rect);
    dl->AddText(font, font->FontSize, ImVec2(current_x, current_y), text_color, text.GetCharArray(),
                text.GetCharArray() + text.GetLength(), 0.0f, &clip_rect);

    current_y += font->FontSize + spacing;
  }
}

namespace SaveStateSelectorUI {
struct ListEntry
{
  std::string path;
  std::string game_code;
  std::string title;
  std::string formatted_timestamp;
  std::unique_ptr<HostDisplayTexture> preview_texture;
  s32 slot;
  bool global;
};

static void InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot, bool global);
static void InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, std::string path, s32 slot, bool global);

static void RefreshList();
static void RefreshHotkeyLegend();

std::string m_load_legend;
std::string m_save_legend;
std::string m_prev_legend;
std::string m_next_legend;

std::vector<ListEntry> m_slots;
u32 m_current_selection = 0;

Common::Timer m_open_timer;
float m_open_time = 0.0f;
} // namespace SaveStateSelectorUI

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
  m_open_timer.Reset();
  m_open_time = open_time;

  if (s_save_state_selector_ui_open)
    return;

  s_save_state_selector_ui_open = true;
  RefreshList();
  RefreshHotkeyLegend();
}

void SaveStateSelectorUI::Close()
{
  if (!s_save_state_selector_ui_open)
    return;

  s_save_state_selector_ui_open = false;
  m_load_legend = {};
  m_save_legend = {};
  m_prev_legend = {};
  m_next_legend = {};
  m_slots.clear();
}

void SaveStateSelectorUI::RefreshList()
{
  if (!System::GetRunningCode().empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::string path(System::GetGameSaveStateFileName(System::GetRunningCode(), i));
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, false);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, false);

      m_slots.push_back(std::move(li));
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

    m_slots.push_back(std::move(li));
  }

  if (m_slots.empty() || m_current_selection >= m_slots.size())
    m_current_selection = 0;
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  auto format_legend_entry = [](std::string_view setting, std::string_view caption) {
    auto slash_pos = setting.find_first_of('/');
    if (slash_pos != setting.npos)
    {
      setting = setting.substr(slash_pos + 1);
    }

    return StringUtil::StdStringFromFormat("%.*s - %.*s", static_cast<int>(setting.size()), setting.data(),
                                           static_cast<int>(caption.size()), caption.data());
  };

  m_load_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "LoadSelectedSaveState"),
                                      Host::TranslateStdString("SaveStateSelectorUI", "Load"));
  m_save_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SaveSelectedSaveState"),
                                      Host::TranslateStdString("SaveStateSelectorUI", "Save"));
  m_prev_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SelectPreviousSaveStateSlot"),
                                      Host::TranslateStdString("SaveStateSelectorUI", "Select Previous"));
  m_next_legend = format_legend_entry(Host::GetStringSettingValue("Hotkeys", "SelectNextSaveStateSlot"),
                                      Host::TranslateStdString("SaveStateSelectorUI", "Select Next"));
}

void SaveStateSelectorUI::SelectNextSlot()
{
  if (!s_save_state_selector_ui_open)
    Open();

  m_open_timer.Reset();
  m_current_selection = (m_current_selection == static_cast<u32>(m_slots.size() - 1)) ? 0 : (m_current_selection + 1);
}

void SaveStateSelectorUI::SelectPreviousSlot()
{
  if (!s_save_state_selector_ui_open)
    Open();

  m_open_timer.Reset();
  m_current_selection =
    (m_current_selection == 0) ? (static_cast<u32>(m_slots.size()) - 1u) : (m_current_selection - 1);
}

void SaveStateSelectorUI::InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, std::string path, s32 slot,
                                              bool global)
{
  li->title = std::move(ssi->title);
  li->game_code = std::move(ssi->game_code);
  li->path = std::move(path);
  li->formatted_timestamp = fmt::format("{:%c}", fmt::localtime(ssi->timestamp));
  li->slot = slot;
  li->global = global;

  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture = g_host_display->CreateTexture(ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1,
                                                        HostDisplayPixelFormat::RGBA8, ssi->screenshot_data.data(),
                                                        sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = g_host_display->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1,
                                                        HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
                                                        sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  }

  if (!li->preview_texture)
    Log_ErrorPrintf("Failed to upload save state image to GPU");
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, std::string path, s32 slot, bool global)
{
  li->title = Host::TranslateStdString("SaveStateSelectorUI", "No Save State");
  std::string().swap(li->game_code);
  li->path = std::move(path);
  std::string().swap(li->formatted_timestamp);
  li->slot = slot;
  li->global = global;

  li->preview_texture = g_host_display->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1,
                                                      HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
                                                      sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);

  if (!li->preview_texture)
    Log_ErrorPrintf("Failed to upload save state image to GPU");
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

      for (size_t i = 0; i < m_slots.size(); i++)
      {
        const ListEntry& entry = m_slots[i];
        const float y_start = item_height * static_cast<float>(i);

        if (i == m_current_selection)
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
          ImGui::Image(reinterpret_cast<ImTextureID>(entry.preview_texture->GetHandle()), image_size);
        }

        ImGui::SetCursorPosY(y_start + padding);

        ImGui::Indent(text_indent);

        if (entry.global)
        {
          ImGui::Text(Host::TranslateString("SaveStateSelectorUI", "Global Slot %d"), entry.slot);
        }
        else if (entry.game_code.empty())
        {
          ImGui::Text(Host::TranslateString("SaveStateSelectorUI", "Game Slot %d"), entry.slot);
        }
        else
        {
          ImGui::Text(Host::TranslateString("SaveStateSelectorUI", "%s Slot %d"), entry.game_code.c_str(), entry.slot);
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
      ImGui::TextUnformatted(m_load_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m_prev_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m_save_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m_next_legend.c_str());

      ImGui::EndTable();
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  // auto-close
  if (m_open_timer.GetTimeSeconds() >= m_open_time)
    Close();
}

void SaveStateSelectorUI::LoadCurrentSlot()
{
  if (m_slots.empty() || m_current_selection >= m_slots.size() || m_slots[m_current_selection].path.empty())
    return;

  System::LoadState(m_slots[m_current_selection].path.c_str());
  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  if (m_slots.empty() || m_current_selection >= m_slots.size() || m_slots[m_current_selection].path.empty())
    return;

  System::SaveState(m_slots[m_current_selection].path.c_str(), g_settings.create_save_state_backups);
  Close();
}
