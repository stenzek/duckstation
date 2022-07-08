#include "save_state_selector_ui.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host_display.h"
#include "core/system.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "icon.h"
#include "imgui.h"
Log_SetChannel(SaveStateSelectorUI);

namespace FrontendCommon {

SaveStateSelectorUI::SaveStateSelectorUI(CommonHostInterface* host_interface) : m_host_interface(host_interface) {}

SaveStateSelectorUI::~SaveStateSelectorUI() = default;

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
  m_open_timer.Reset();
  m_open_time = open_time;

  if (m_open)
    return;

  m_open = true;
  RefreshList();
  RefreshHotkeyLegend();
}

void SaveStateSelectorUI::Close()
{
  if (!m_open)
    return;

  m_open = false;
  ClearList();
}

void SaveStateSelectorUI::ClearList()
{
  m_slots.clear();
}

void SaveStateSelectorUI::RefreshList()
{
  ClearList();

  if (!System::GetRunningCode().empty())
  {
    for (s32 i = 1; i <= CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
        m_host_interface->GetExtendedSaveStateInfo(System::GetRunningCode().c_str(), i);

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value());
      else
        InitializePlaceholderListEntry(&li, i, false);

      m_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
      m_host_interface->GetExtendedSaveStateInfo(nullptr, i);

    ListEntry li;
    if (ssi)
      InitializeListEntry(&li, &ssi.value());
    else
      InitializePlaceholderListEntry(&li, i, true);

    m_slots.push_back(std::move(li));
  }

  if (m_slots.empty() || m_current_selection >= m_slots.size())
    m_current_selection = 0;
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  if (!m_open)
    return;

  auto format_legend_entry = [](std::string_view setting, std::string_view caption) {
    auto slash_pos = setting.find_first_of('/');
    if (slash_pos != setting.npos)
    {
      setting = setting.substr(slash_pos + 1);
    }

    return StringUtil::StdStringFromFormat("%.*s - %.*s", static_cast<int>(setting.size()), setting.data(),
                                           static_cast<int>(caption.size()), caption.data());
  };

  m_load_legend = format_legend_entry(m_host_interface->GetStringSettingValue("Hotkeys", "LoadSelectedSaveState"),
                                      m_host_interface->TranslateStdString("SaveStateSelectorUI", "Load"));
  m_save_legend = format_legend_entry(m_host_interface->GetStringSettingValue("Hotkeys", "SaveSelectedSaveState"),
                                      m_host_interface->TranslateStdString("SaveStateSelectorUI", "Save"));
  m_prev_legend = format_legend_entry(m_host_interface->GetStringSettingValue("Hotkeys", "SelectPreviousSaveStateSlot"),
                                      m_host_interface->TranslateStdString("SaveStateSelectorUI", "Select Previous"));
  m_next_legend = format_legend_entry(m_host_interface->GetStringSettingValue("Hotkeys", "SelectNextSaveStateSlot"),
                                      m_host_interface->TranslateStdString("SaveStateSelectorUI", "Select Next"));
}

const char* SaveStateSelectorUI::GetSelectedStatePath() const
{
  if (m_slots.empty() || m_slots[m_current_selection].path.empty())
    return nullptr;

  return m_slots[m_current_selection].path.c_str();
}

s32 SaveStateSelectorUI::GetSelectedStateSlot() const
{
  if (m_slots.empty())
    return 0;

  return m_slots[m_current_selection].slot;
}

void SaveStateSelectorUI::SelectNextSlot()
{
  if (!m_open)
    Open();

  ResetOpenTimer();
  m_current_selection = (m_current_selection == static_cast<u32>(m_slots.size() - 1)) ? 0 : (m_current_selection + 1);
}

void SaveStateSelectorUI::SelectPreviousSlot()
{
  if (!m_open)
    Open();

  ResetOpenTimer();
  m_current_selection =
    (m_current_selection == 0) ? (static_cast<u32>(m_slots.size()) - 1u) : (m_current_selection - 1);
}

void SaveStateSelectorUI::InitializeListEntry(ListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi)
{
  li->title = std::move(ssi->title);
  li->game_code = std::move(ssi->game_code);
  li->path = std::move(ssi->path);
  li->formatted_timestamp = fmt::format("{:%c}", fmt::localtime(ssi->timestamp));
  li->slot = ssi->slot;
  li->global = ssi->global;

  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture = m_host_interface->GetDisplay()->CreateTexture(
      ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, HostDisplayPixelFormat::RGBA8,
      ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = m_host_interface->GetDisplay()->CreateTexture(
      PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
      sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  }

  if (!li->preview_texture)
    Log_ErrorPrintf("Failed to upload save state image to GPU");
}

std::pair<s32, bool> SaveStateSelectorUI::GetSlotTypeFromSelection(u32 selection) const
{
  if (selection < CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS)
  {
    return {selection + 1, false};
  }

  return {selection - CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS + 1, true};
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, s32 slot, bool global)
{
  li->title = m_host_interface->TranslateStdString("SaveStateSelectorUI", "No Save State");
  std::string().swap(li->game_code);
  std::string().swap(li->path);
  std::string().swap(li->formatted_timestamp);
  li->slot = slot;
  li->global = global;

  li->preview_texture = m_host_interface->GetDisplay()->CreateTexture(
    PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
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
          ImGui::Text(m_host_interface->TranslateString("SaveStateSelectorUI", "Global Slot %d"), entry.slot);
        }
        else if (entry.game_code.empty())
        {
          ImGui::Text(m_host_interface->TranslateString("SaveStateSelectorUI", "Game Slot %d"), entry.slot);
        }
        else
        {
          ImGui::Text(m_host_interface->TranslateString("SaveStateSelectorUI", "%s Slot %d"), entry.game_code.c_str(),
                      entry.slot);
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

      const bool hide_load_button = m_host_interface->IsCheevosChallengeModeActive();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(!hide_load_button ? m_load_legend.c_str() : m_save_legend.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m_prev_legend.c_str());
      ImGui::TableNextColumn();
      if (!hide_load_button)
      {
        ImGui::TextUnformatted(m_save_legend.c_str());
      }
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
  const auto slot_info = GetSlotTypeFromSelection(m_current_selection);
  m_host_interface->LoadState(slot_info.second, slot_info.first);
  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  const auto slot_info = GetSlotTypeFromSelection(m_current_selection);
  m_host_interface->SaveState(slot_info.second, slot_info.first);
  Close();
}

} // namespace FrontendCommon
