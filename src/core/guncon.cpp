// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "guncon.h"
#include "gpu.h"
#include "host.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsPromptFont.h"

#include <array>

#ifdef _DEBUG
#include "common/log.h"
Log_SetChannel(GunCon);
#endif

static constexpr std::array<u8, static_cast<size_t>(GunCon::Binding::ButtonCount)> s_button_indices = {{13, 3, 14}};

GunCon::GunCon(u32 index) : Controller(index)
{
}

GunCon::~GunCon()
{
  if (!m_cursor_path.empty())
  {
    const u32 cursor_index = GetSoftwarePointerIndex();
    if (cursor_index < InputManager::MAX_SOFTWARE_CURSORS)
      ImGuiManager::ClearSoftwareCursor(cursor_index);
  }
}

ControllerType GunCon::GetType() const
{
  return ControllerType::GunCon;
}

void GunCon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool GunCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  u16 position_x = m_position_x;
  u16 position_y = m_position_y;
  sw.Do(&button_state);
  sw.Do(&position_x);
  sw.Do(&position_y);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_position_x = position_x;
    m_position_y = position_y;
  }

  sw.Do(&m_transfer_state);
  return true;
}

float GunCon::GetBindState(u32 index) const
{
  if (index >= s_button_indices.size())
    return 0.0f;

  const u32 bit = s_button_indices[index];
  return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
}

void GunCon::SetBindState(u32 index, float value)
{
  const bool pressed = (value >= 0.5f);
  if (index == static_cast<u32>(Binding::ShootOffscreen))
  {
    if (m_shoot_offscreen != pressed)
    {
      m_shoot_offscreen = pressed;
      SetBindState(static_cast<u32>(Binding::Trigger), pressed);
    }

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
    m_button_state &= ~(u16(1) << s_button_indices[static_cast<u8>(index)]);
  else
    m_button_state |= u16(1) << s_button_indices[static_cast<u8>(index)];
}

void GunCon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool GunCon::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A63;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = Truncate8(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::XLSB;
      return true;
    }

    case TransferState::XLSB:
    {
      UpdatePosition();
      *data_out = Truncate8(m_position_x);
      m_transfer_state = TransferState::XMSB;
      return true;
    }

    case TransferState::XMSB:
    {
      *data_out = Truncate8(m_position_x >> 8);
      m_transfer_state = TransferState::YLSB;
      return true;
    }

    case TransferState::YLSB:
    {
      *data_out = Truncate8(m_position_y);
      m_transfer_state = TransferState::YMSB;
      return true;
    }

    case TransferState::YMSB:
    {
      *data_out = Truncate8(m_position_y >> 8);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
      UnreachableCode();
  }
}

void GunCon::UpdatePosition()
{
  float display_x, display_y;
  const auto& [window_x, window_y] = (m_has_relative_binds) ? GetAbsolutePositionFromRelativeAxes() :
                                                              InputManager::GetPointerAbsolutePosition(m_cursor_index);
  g_gpu->ConvertScreenCoordinatesToDisplayCoordinates(window_x, window_y, &display_x, &display_y);

  // are we within the active display area?
  u32 tick, line;
  if (display_x < 0 || display_y < 0 ||
      !g_gpu->ConvertDisplayCoordinatesToBeamTicksAndLines(display_x, display_y, m_x_scale, &tick, &line) ||
      m_shoot_offscreen)
  {
    DEBUG_LOG("Lightgun out of range for window coordinates {:.0f},{:.0f}", window_x, window_y);
    m_position_x = 0x01;
    m_position_y = 0x0A;
    return;
  }

  // 8MHz units for X = 44100*768*11/7 = 53222400 / 8000000 = 6.6528
  const double divider = static_cast<double>(g_gpu->GetCRTCFrequency()) / 8000000.0;
  m_position_x = static_cast<u16>(static_cast<float>(tick) / static_cast<float>(divider));
  m_position_y = static_cast<u16>(line);
  DEBUG_LOG("Lightgun window coordinates {:.0f},{:.0f} -> tick {} line {} 8mhz ticks {}", display_x, display_y, tick,
            line, m_position_x);
}

std::pair<float, float> GunCon::GetAbsolutePositionFromRelativeAxes() const
{
  const float screen_rel_x = (((m_relative_pos[1] > 0.0f) ? m_relative_pos[1] : -m_relative_pos[0]) + 1.0f) * 0.5f;
  const float screen_rel_y = (((m_relative_pos[3] > 0.0f) ? m_relative_pos[3] : -m_relative_pos[2]) + 1.0f) * 0.5f;
  return std::make_pair(screen_rel_x * ImGuiManager::GetWindowWidth(), screen_rel_y * ImGuiManager::GetWindowHeight());
}

bool GunCon::CanUseSoftwareCursor() const
{
  return (InputManager::MAX_POINTER_DEVICES + m_index) < InputManager::MAX_SOFTWARE_CURSORS;
}

u32 GunCon::GetSoftwarePointerIndex() const
{
  return m_has_relative_binds ? (InputManager::MAX_POINTER_DEVICES + m_index) : m_cursor_index;
}

void GunCon::UpdateSoftwarePointerPosition()
{
  if (m_cursor_path.empty() || !CanUseSoftwareCursor())
    return;

  const auto& [window_x, window_y] = GetAbsolutePositionFromRelativeAxes();
  ImGuiManager::SetSoftwareCursorPosition(GetSoftwarePointerIndex(), window_x, window_y);
}

std::unique_ptr<GunCon> GunCon::Create(u32 index)
{
  return std::make_unique<GunCon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, binding, genb)                                                           \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::Button, genb                     \
  }
#define HALFAXIS(name, display_name, icon_name, binding, genb)                                                         \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::HalfAxis, genb                   \
  }

  // clang-format off
  {"Pointer", TRANSLATE_NOOP("GunCon", "Pointer/Aiming"), ICON_PF_MOUSE, static_cast<u32>(GunCon::Binding::ButtonCount), InputBindingInfo::Type::Pointer, GenericInputBinding::Unknown},
  BUTTON("Trigger", TRANSLATE_NOOP("GunCon", "Trigger"), ICON_PF_CROSS, GunCon::Binding::Trigger, GenericInputBinding::R2),
  BUTTON("ShootOffscreen", TRANSLATE_NOOP("GunCon", "Shoot Offscreen"), nullptr, GunCon::Binding::ShootOffscreen, GenericInputBinding::L2),
  BUTTON("A", TRANSLATE_NOOP("GunCon", "A"), ICON_PF_BUTTON_A, GunCon::Binding::A, GenericInputBinding::Cross),
  BUTTON("B", TRANSLATE_NOOP("GunCon", "B"), ICON_PF_BUTTON_B, GunCon::Binding::B, GenericInputBinding::Circle),

  HALFAXIS("RelativeLeft", TRANSLATE_NOOP("GunCon", "Relative Left"), ICON_PF_ANALOG_LEFT, GunCon::Binding::RelativeLeft, GenericInputBinding::Unknown),
  HALFAXIS("RelativeRight", TRANSLATE_NOOP("GunCon", "Relative Right"), ICON_PF_ANALOG_RIGHT, GunCon::Binding::RelativeRight, GenericInputBinding::Unknown),
  HALFAXIS("RelativeUp", TRANSLATE_NOOP("GunCon", "Relative Up"), ICON_PF_ANALOG_UP, GunCon::Binding::RelativeUp, GenericInputBinding::Unknown),
  HALFAXIS("RelativeDown", TRANSLATE_NOOP("GunCon", "Relative Down"), ICON_PF_ANALOG_DOWN, GunCon::Binding::RelativeDown, GenericInputBinding::Unknown),
// clang-format on

#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Path, "CrosshairImagePath", TRANSLATE_NOOP("GunCon", "Crosshair Image Path"),
   TRANSLATE_NOOP("GunCon", "Path to an image to use as a crosshair/cursor."), nullptr, nullptr, nullptr, nullptr,
   nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "CrosshairScale", TRANSLATE_NOOP("GunCon", "Crosshair Image Scale"),
   TRANSLATE_NOOP("GunCon", "Scale of crosshair image on screen."), "1.0", "0.0001", "100.0", "0.10", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::String, "CrosshairColor", TRANSLATE_NOOP("GunCon", "Cursor Color"),
   TRANSLATE_NOOP("GunCon", "Applies a color to the chosen crosshair images, can be used for multiple players. Specify "
                            "in HTML/CSS format (e.g. #aabbcc)"),
   "#ffffff", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "XScale", TRANSLATE_NOOP("GunCon", "X Scale"),
   TRANSLATE_NOOP("GunCon", "Scales X coordinates relative to the center of the screen."), "1.0", "0.01", "2.0", "0.01",
   "%.0f%%", nullptr, 100.0f}};

const Controller::ControllerInfo GunCon::INFO = {
  ControllerType::GunCon, "GunCon",   TRANSLATE_NOOP("ControllerType", "GunCon"),    ICON_PF_LIGHT_GUN,
  s_binding_info,         s_settings, Controller::VibrationCapabilities::NoVibration};

void GunCon::LoadSettings(SettingsInterface& si, const char* section, bool initial)
{
  Controller::LoadSettings(si, section, initial);

  m_x_scale = si.GetFloatValue(section, "XScale", 1.0f);

  std::string cursor_path = si.GetStringValue(section, "CrosshairImagePath");
  const float cursor_scale = si.GetFloatValue(section, "CrosshairScale", 1.0f);
  u32 cursor_color = 0xFFFFFF;
  if (std::string cursor_color_str = si.GetStringValue(section, "CrosshairColor", ""); !cursor_color_str.empty())
  {
    // Strip the leading hash, if it's a CSS style colour.
    const std::optional<u32> cursor_color_opt(StringUtil::FromChars<u32>(
      cursor_color_str[0] == '#' ? std::string_view(cursor_color_str).substr(1) : std::string_view(cursor_color_str),
      16));
    if (cursor_color_opt.has_value())
      cursor_color = cursor_color_opt.value();
  }

#ifndef __ANDROID__
  if (cursor_path.empty())
    cursor_path = Path::Combine(EmuFolders::Resources, "images/crosshair.png");
#endif

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
    const bool had_software_cursor = m_cursor_path.empty();
    m_cursor_path = std::move(cursor_path);
    m_cursor_scale = cursor_scale;
    m_cursor_color = cursor_color;
    if (static_cast<u32>(new_pointer_index) < InputManager::MAX_SOFTWARE_CURSORS)
    {
      if (!m_cursor_path.empty())
      {
        ImGuiManager::SetSoftwareCursor(new_pointer_index, m_cursor_path, m_cursor_scale, m_cursor_color);
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
