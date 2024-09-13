// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "playstation_mouse.h"
#include "gpu.h"
#include "host.h"
#include "system.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"

#include "IconsPromptFont.h"

#include <array>

Log_SetChannel(PlayStationMouse);

static constexpr std::array<u8, static_cast<size_t>(PlayStationMouse::Binding::ButtonCount)> s_button_indices = {
  {11, 10}};

PlayStationMouse::PlayStationMouse(u32 index) : Controller(index)
{
}

PlayStationMouse::~PlayStationMouse() = default;

ControllerType PlayStationMouse::GetType() const
{
  return ControllerType::PlayStationMouse;
}

void PlayStationMouse::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  float delta_x = m_delta_x;
  float delta_y = m_delta_y;
  sw.Do(&button_state);
  if (sw.GetVersion() >= 60) [[unlikely]]
  {
    sw.Do(&delta_x);
    sw.Do(&delta_y);
  }
  else
  {
    u8 dummy = 0;
    sw.Do(&dummy);
    sw.Do(&dummy);
  }

  if (apply_input_state)
  {
    m_button_state = button_state;
    m_delta_x = delta_x;
    m_delta_y = delta_y;
  }

  sw.Do(&m_transfer_state);
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

void PlayStationMouse::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A12;

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
      m_transfer_state = TransferState::DeltaX;
      return true;
    }

    case TransferState::DeltaX:
    {
      const float delta_x =
        std::clamp(std::floor(m_delta_x * m_sensitivity_x), static_cast<float>(std::numeric_limits<s8>::min()),
                   static_cast<float>(std::numeric_limits<s8>::max()));
      m_delta_x -= delta_x / m_sensitivity_x;
      *data_out = static_cast<s8>(delta_x);
      m_transfer_state = TransferState::DeltaY;
      return true;
    }

    case TransferState::DeltaY:
    {
      const float delta_y =
        std::clamp(std::floor(m_delta_y * m_sensitivity_y), static_cast<float>(std::numeric_limits<s8>::min()),
                   static_cast<float>(std::numeric_limits<s8>::max()));
      m_delta_y -= delta_y / m_sensitivity_x;
      *data_out = static_cast<s8>(delta_y);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
    }
  }
}

void PlayStationMouse::LoadSettings(SettingsInterface& si, const char* section, bool initial)
{
  Controller::LoadSettings(si, section, initial);

  m_sensitivity_x = si.GetFloatValue(section, "SensitivityX", 1.0f);
  m_sensitivity_y = si.GetFloatValue(section, "SensitivityY", 1.0f);
}

std::unique_ptr<PlayStationMouse> PlayStationMouse::Create(u32 index)
{
  return std::make_unique<PlayStationMouse>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb                      \
  }

  // clang-format off
  { "Pointer", TRANSLATE_NOOP("PlaystationMouse", "Pointer"), ICON_PF_MOUSE_ANY, static_cast<u32>(PlayStationMouse::Binding::PointerX), InputBindingInfo::Type::Pointer, GenericInputBinding::Unknown },
  BUTTON("Left", TRANSLATE_NOOP("PlayStationMouse", "Left Button"), ICON_PF_MOUSE_BUTTON_1, PlayStationMouse::Binding::Left, GenericInputBinding::Cross),
  BUTTON("Right", TRANSLATE_NOOP("PlayStationMouse", "Right Button"), ICON_PF_MOUSE_BUTTON_2, PlayStationMouse::Binding::Right, GenericInputBinding::Circle),
// clang-format on

#undef BUTTON
};
static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "SensitivityX", TRANSLATE_NOOP("PlayStationMouse", "Horizontal Sensitivity"),
   TRANSLATE_NOOP("PlayStationMouse", "Adjusts the correspondance between physical and virtual mouse movement."), "1.0",
   "0.01", "2.0", "0.01", "%.0f", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SensitivityY", TRANSLATE_NOOP("PlayStationMouse", "Vertical Sensitivity"),
   TRANSLATE_NOOP("PlayStationMouse", "Adjusts the correspondance between physical and virtual mouse movement."), "1.0",
   "0.01", "2.0", "0.01", "%.0f", nullptr, 100.0f},
};

const Controller::ControllerInfo PlayStationMouse::INFO = {ControllerType::PlayStationMouse,
                                                           "PlayStationMouse",
                                                           TRANSLATE_NOOP("ControllerType", "Mouse"),
                                                           ICON_PF_MOUSE,
                                                           s_binding_info,
                                                           s_settings,
                                                           Controller::VibrationCapabilities::NoVibration};