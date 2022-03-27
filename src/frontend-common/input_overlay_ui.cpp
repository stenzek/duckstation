#include "input_overlay_ui.h"
#include "common_host_interface.h"
#include "core/imgui_fullscreen.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/system.h"
#include "fullscreen_ui.h"

static CommonHostInterface* GetHostInterface()
{
  return static_cast<CommonHostInterface*>(g_host_interface);
}

namespace FrontendCommon {

InputOverlayUI::InputOverlayUI() = default;

InputOverlayUI::~InputOverlayUI() = default;

void InputOverlayUI::Draw()
{
  UpdateNames();

  if (m_active_ports == 0)
    return;

  ImFont* font;
  float margin, spacing, shadow_offset;

  if (GetHostInterface()->IsFullscreenUIEnabled())
  {
    font = ImGuiFullscreen::g_large_font;
    margin = ImGuiFullscreen::LayoutScale(10.0f);
    spacing = ImGuiFullscreen::LayoutScale(5.0f);
    shadow_offset = ImGuiFullscreen::LayoutScale(1.0f);
  }
  else
  {
    font = ImGui::GetFont();
    margin = ImGuiFullscreen::DPIScale(10.0f);
    spacing = ImGuiFullscreen::DPIScale(5.0f);
    shadow_offset = ImGuiFullscreen::DPIScale(1.0f);
  }

  static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);
  static constexpr u32 shadow_color = IM_COL32(0x00, 0x00, 0x00, 100);

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  float current_x = margin;
  float current_y =
    display_size.y - margin - ((static_cast<float>(m_active_ports) * (font->FontSize + spacing)) - spacing);

  const ImVec4 clip_rect(current_x, current_y, display_size.x - margin, display_size.y - margin);

  LargeString text;

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (m_types[port] == ControllerType::None)
      continue;

    const Controller* controller = g_pad.GetController(port);
    DebugAssert(controller);

    text.Format("P%u |", port + 1u);

    if (!m_axis_names[port].empty())
    {
      for (const auto& [axis_name, axis_code, axis_type] : m_axis_names[port])
      {
        const float value = controller->GetAxisState(axis_code);
        text.AppendFormattedString(" %s: %.2f", axis_name.c_str(), value);
      }

      text.AppendString(" |");
    }

    for (const auto& [button_name, button_code] : m_button_names[port])
    {
      const bool pressed = controller->GetButtonState(button_code);
      if (pressed)
        text.AppendFormattedString(" %s", button_name.c_str());
    }

    dl->AddText(font, font->FontSize, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
                text.GetCharArray(), text.GetCharArray() + text.GetLength(), 0.0f, &clip_rect);
    dl->AddText(font, font->FontSize, ImVec2(current_x, current_y), text_color, text.GetCharArray(),
                text.GetCharArray() + text.GetLength(), 0.0f, &clip_rect);

    current_y += font->FontSize + spacing;
  }
}

void InputOverlayUI::UpdateNames()
{
  m_active_ports = 0;
  if (!System::IsValid())
    return;

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    const Controller* controller = g_pad.GetController(port);
    const ControllerType type = (controller) ? controller->GetType() : ControllerType::None;
    if (type != ControllerType::None)
      m_active_ports++;

    if (type == m_types[port])
      continue;

    m_axis_names[port] = Controller::GetAxisNames(type);
    m_button_names[port] = Controller::GetButtonNames(type);
    m_types[port] = type;
  }
}

} // namespace FrontendCommon