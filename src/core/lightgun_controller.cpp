// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "lightgun_controller.h"
#include "gpu.h"
#include "system.h"
#include "video_thread.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/path.h"
#include "common/settings_interface.h"
#include "common/string_util.h"

static constexpr const char* DEFAULT_CROSSHAIR_PATH = "images" FS_OSPATH_SEPARATOR_STR "crosshair.png";

LightgunController::LightgunController(u32 index, const std::array<u8, 4>& button_indices)
  : Controller(index), m_button_indices(button_indices)
{
}

LightgunController::~LightgunController()
{
  if (!m_cursor_path.empty())
  {
    const u32 cursor_index = GetSoftwarePointerIndex();
    if (cursor_index < InputManager::MAX_SOFTWARE_CURSORS)
      ImGuiManager::ClearSoftwareCursor(cursor_index);
  }
}

bool LightgunController::DoState(StateWrapper& sw, bool apply_input_state)
{
  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  return true;
}

float LightgunController::GetBindState(u32 index) const
{
  if (index >= m_button_indices.size())
    return 0.0f;

  const u32 bit = m_button_indices[index];
  return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
}

void LightgunController::SetBindState(u32 index, float value)
{
  const bool pressed = (value >= 0.5f);
  if (index == static_cast<u32>(Binding::ShootOffscreen))
  {
    SetShootOffscreen(pressed);
    return;
  }
  else if (index >= static_cast<u32>(Binding::ButtonCount))
  {
    if (index >= static_cast<u32>(Binding::BindingCount) || !m_has_relative_binds)
      return;

    if (m_relative_pos[index - static_cast<u32>(Binding::RelativeLeft)] != value)
    {
      m_relative_pos[index - static_cast<u32>(Binding::RelativeLeft)] = value;
      UpdateSoftwarePointerPosition();
    }

    return;
  }

  if (pressed)
    m_button_state &= ~(u16(1) << m_button_indices[static_cast<u8>(index)]);
  else
    m_button_state |= u16(1) << m_button_indices[static_cast<u8>(index)];
}

u32 LightgunController::GetButtonStateBits() const
{
  return m_button_state;
}

LightgunController::Position LightgunController::GetPosition() const
{
  const auto [window_x, window_y] = (m_has_relative_binds) ? GetAbsolutePositionFromRelativeAxes() :
                                                             InputManager::GetPointerAbsolutePosition(m_cursor_index);
  const GSVector2 display_pos = GPU::ConvertScreenCoordinatesToDisplayCoordinates(GSVector2(window_x, window_y));

  Position ret = {window_x, window_y, display_pos.x, display_pos.y, 0, 0, false};
  ret.valid = !(display_pos < GSVector2::zero()).anytrue() &&
              GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(display_pos, m_x_scale, &ret.tick, &ret.line);
  return ret;
}

// Common lightgun input and cursor handling is implemented by LightgunController.

std::pair<float, float> LightgunController::GetAbsolutePositionFromRelativeAxes() const
{
  const float screen_rel_x = (((m_relative_pos[1] > 0.0f) ? m_relative_pos[1] : -m_relative_pos[0]) + 1.0f) * 0.5f;
  const float screen_rel_y = (((m_relative_pos[3] > 0.0f) ? m_relative_pos[3] : -m_relative_pos[2]) + 1.0f) * 0.5f;
  const WindowInfo& wi = VideoThread::GetRenderWindowInfo();
  return std::make_pair(screen_rel_x * static_cast<float>(wi.surface_width),
                        screen_rel_y * static_cast<float>(wi.surface_height));
}

bool LightgunController::CanUseSoftwareCursor() const
{
  return (InputManager::MAX_POINTER_DEVICES + m_index) < InputManager::MAX_SOFTWARE_CURSORS;
}

u32 LightgunController::GetSoftwarePointerIndex() const
{
  return m_has_relative_binds ? (InputManager::MAX_POINTER_DEVICES + m_index) : m_cursor_index;
}

void LightgunController::UpdateSoftwarePointerPosition()
{
  if (m_cursor_path.empty() || !CanUseSoftwareCursor())
    return;

  const auto& [window_x, window_y] = GetAbsolutePositionFromRelativeAxes();
  ImGuiManager::SetSoftwareCursorPosition(GetSoftwarePointerIndex(), window_x, window_y);
}

void LightgunController::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_x_scale = si.GetFloatValue(section, "XScale", 1.0f);

  std::string cursor_path = si.GetStringValue(section, "CrosshairImagePath", DEFAULT_CROSSHAIR_PATH);
  const float cursor_scale = si.GetFloatValue(section, "CrosshairScale", 1.0f);
  u32 cursor_color = 0xFFFFFF;
  if (std::string cursor_color_str = si.GetStringValue(section, "CrosshairColor", ""); !cursor_color_str.empty())
  {
    // Strip the leading hash, if it's a CSS style colour.
    const std::optional<u32> cursor_color_opt(StringUtil::FromChars<u32>(
      cursor_color_str[0] == '#' ? std::string_view(cursor_color_str).substr(1) : std::string_view(cursor_color_str),
      16));
    if (cursor_color_opt.has_value())
    {
      cursor_color = cursor_color_opt.value();
      cursor_color = (cursor_color & 0x00FF00u) | ((cursor_color >> 16) & 0xFFu) | ((cursor_color & 0xFFu) << 16);
    }
  }

  const s32 prev_pointer_index = GetSoftwarePointerIndex();

  m_has_relative_binds = (si.ContainsValue(section, "RelativeLeft") || si.ContainsValue(section, "RelativeRight") ||
                          si.ContainsValue(section, "RelativeUp") || si.ContainsValue(section, "RelativeDown"));
  m_cursor_index =
    static_cast<u8>(InputManager::GetIndexFromPointerBinding(si.GetStringValue(section, "Pointer")).value_or(0));

  const s32 new_pointer_index = GetSoftwarePointerIndex();

  if (prev_pointer_index != new_pointer_index || m_cursor_path != cursor_path || m_cursor_scale != cursor_scale ||
      m_cursor_color != cursor_color)
  {
    if (!initial && prev_pointer_index != new_pointer_index &&
        static_cast<u32>(prev_pointer_index) < InputManager::MAX_SOFTWARE_CURSORS)
    {
      ImGuiManager::ClearSoftwareCursor(prev_pointer_index);
    }

    // Pointer changed, so need to update software cursor.
    const bool had_software_cursor = !m_cursor_path.empty();
    m_cursor_path = std::move(cursor_path);
    m_cursor_scale = cursor_scale;
    m_cursor_color = cursor_color;
    if (static_cast<u32>(new_pointer_index) < InputManager::MAX_SOFTWARE_CURSORS)
    {
      if (!m_cursor_path.empty())
      {
        std::string image_path;
        if (!Path::IsAbsolute(m_cursor_path))
          image_path = Path::Combine(EmuFolders::Resources, m_cursor_path);
        else
          image_path = m_cursor_path;

        ImGuiManager::SetSoftwareCursor(new_pointer_index, std::move(image_path), m_cursor_scale, m_cursor_color);
        if (m_has_relative_binds)
          UpdateSoftwarePointerPosition();
      }
      else if (had_software_cursor)
      {
        ImGuiManager::ClearSoftwareCursor(new_pointer_index);
      }
    }
  }
}
