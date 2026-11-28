// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "playstation_mouse.h"
#include "gpu.h"
#include "system.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/settings_interface.h"

#include "IconsPromptFont.h"

#include <array>
#include <cmath>

LOG_CHANNEL(Controller);

static constexpr std::array<u8, static_cast<size_t>(PlayStationMouse::Binding::ButtonCount)> s_button_indices = {
  {11, 10}};

PlayStationMouse::PlayStationMouse(u32 index) : Controller(index, ControllerType::PlayStationMouse)
{
}

PlayStationMouse::~PlayStationMouse() = default;

void PlayStationMouse::Reset()
{
  ResetTransferState();
}

bool PlayStationMouse::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || (apply_input_state && sw.GetVersion() >= 60))
  {
    sw.Do(&m_button_state);
    sw.Do(&m_delta_x);
    sw.Do(&m_delta_y);
  }
  else
  {
    sw.SkipBytes((sw.GetVersion() < 60) ? (sizeof(m_button_state) + sizeof(u8) + sizeof(u8)) :
                                          (sizeof(m_button_state) + sizeof(m_delta_x) + sizeof(m_delta_y)));
  }

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

  return true;
}

float PlayStationMouse::GetBindState(u32 index) const
{
  if (index >= s_button_indices.size())
    return 0.0f;

  const u32 bit = s_button_indices[index];
  return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
}

void PlayStationMouse::SetBindState(u32 index, float value)
{
  if (index >= s_button_indices.size())
  {
    if (index == static_cast<u32>(Binding::PointerX))
      m_delta_x += value;
    else if (index == static_cast<u32>(Binding::PointerY))
      m_delta_y += value;

    return;
  }

  if (value >= 0.5f)
    m_button_state &= ~(u16(1) << s_button_indices[index]);
  else
    m_button_state |= u16(1) << s_button_indices[index];
}

u32 PlayStationMouse::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

u8 PlayStationMouse::GetIDByte() const
{
  return 0x12;
}

bool PlayStationMouse::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  m_tx_buffer[2] = Truncate8(m_button_state);

  // Bits 8-9 always read as 0.
  m_tx_buffer[3] = Truncate8(m_button_state >> 8) & 0xFC;

  const float delta_x = std::floor(m_delta_x * m_sensitivity_x);
  const float delta_y = std::floor(m_delta_y * m_sensitivity_y);
  m_delta_x -= delta_x / m_sensitivity_x;
  m_delta_y -= delta_y / m_sensitivity_y;
  m_tx_buffer[4] = static_cast<u8>(static_cast<s8>(std::clamp(
    delta_x, static_cast<float>(std::numeric_limits<s8>::min()), static_cast<float>(std::numeric_limits<s8>::max()))));
  m_tx_buffer[5] = static_cast<u8>(static_cast<s8>(std::clamp(
    delta_y, static_cast<float>(std::numeric_limits<s8>::min()), static_cast<float>(std::numeric_limits<s8>::max()))));
  return true;
}

void PlayStationMouse::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_sensitivity_x = si.GetFloatValue(section, "SensitivityX", 1.0f);
  m_sensitivity_y = si.GetFloatValue(section, "SensitivityY", 1.0f);
}

std::unique_ptr<PlayStationMouse> PlayStationMouse::Create(u32 index)
{
  return std::make_unique<PlayStationMouse>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}

  // clang-format off
  { "Pointer", TRANSLATE_NOOP("PlayStationMouse", "Pointer"), ICON_PF_MOUSE_ANY, nullptr, static_cast<u32>(PlayStationMouse::Binding::PointerX), InputBindingInfo::Type::RelativePointer, GenericInputBinding::Unknown },
  BUTTON("Left", TRANSLATE_NOOP("PlayStationMouse", "Left Button"), ICON_PF_MOUSE_BUTTON_1, "Left", PlayStationMouse::Binding::Left, GenericInputBinding::Cross),
  BUTTON("Right", TRANSLATE_NOOP("PlayStationMouse", "Right Button"), ICON_PF_MOUSE_BUTTON_2, "Right", PlayStationMouse::Binding::Right, GenericInputBinding::Circle),
// clang-format on

#undef BUTTON
};
static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "SensitivityX", TRANSLATE_NOOP("PlayStationMouse", "Horizontal Sensitivity"),
   TRANSLATE_NOOP("PlayStationMouse", "Adjusts the correspondance between physical and virtual mouse movement."), "1",
   "0.01", "2", "0.01", "%.0f", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SensitivityY", TRANSLATE_NOOP("PlayStationMouse", "Vertical Sensitivity"),
   TRANSLATE_NOOP("PlayStationMouse", "Adjusts the correspondance between physical and virtual mouse movement."), "1",
   "0.01", "2", "0.01", "%.0f", nullptr, 100.0f},
};

const Controller::ControllerInfo PlayStationMouse::INFO = {ControllerType::PlayStationMouse,
                                                           "PlayStationMouse",
                                                           TRANSLATE_NOOP("ControllerType", "Mouse"),
                                                           ICON_PF_MOUSE,
                                                           "images/controllers/mouse.svg",
                                                           s_binding_info,
                                                           s_settings};
