// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "negcon.h"
#include "host.h"
#include "system.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include "IconsPromptFont.h"

#include <array>
#include <cmath>

// Mapping of Button to index of corresponding bit in m_button_state
static constexpr std::array<u8, static_cast<size_t>(NeGcon::Button::Count)> s_button_indices = {3, 4,  5,  6,
                                                                                                7, 11, 12, 13};
NeGcon::NeGcon(u32 index) : Controller(index)
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
}

NeGcon::~NeGcon() = default;

ControllerType NeGcon::GetType() const
{
  return ControllerType::NeGcon;
}

void NeGcon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state);
  return true;
}

float NeGcon::GetBindState(u32 index) const
{
  if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    float value = m_axis_state[static_cast<u32>(Axis::Steering)];
    value = value - 128.0f;
    value /= value < 0.0f ? 128.0f : 127.0f;
    value = std::clamp(value, -1.0f, 1.0f);
    if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)))
    {
      value *= -1.0f;
    }
    return std::max(0.0f, value);
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (static_cast<u32>(Button::Count) + 1);
    if (sub_index >= m_axis_state.size())
      return 0.0f;

    return static_cast<float>(m_axis_state[sub_index]) * (1.0f / 255.0f);
  }
  else
  {
    const u32 bit = s_button_indices[index];
    return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
  }
}

static float apply_axis_modifier(float value, const NeGcon::AxisModifier& axis_modifier)
{
  value = (value - axis_modifier.deadzone) / (axis_modifier.saturation - axis_modifier.deadzone);
  value = std::clamp(value, 0.0f, 1.0f);
  value = std::pow(value, std::exp(axis_modifier.linearity));
  return value;
}

static u8 get_scaled_value(float value, const NeGcon::AxisModifier& axis_modifier)
{
  value = axis_modifier.scaling * axis_modifier.unit * value + axis_modifier.zero;
  return static_cast<u8>(std::clamp(std::round(value), 0.0f, 255.0f));
}

void NeGcon::SetBindState(u32 index, float value)
{
  // Steering Axis: -1..1 -> 0..255
  if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    value = apply_axis_modifier(value, m_steering_modifier);

    m_half_axis_state[index - static_cast<u32>(Button::Count)] = std::clamp(value, 0.0f, 1.0f);

    float merged = m_half_axis_state[1] - m_half_axis_state[0];
    m_axis_state[static_cast<u32>(Axis::Steering)] = get_scaled_value(merged, m_steering_modifier);
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (static_cast<u32>(Button::Count) + 1);
    if (sub_index >= m_axis_state.size())
      return;

    if (index >= (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::I)) &&
        index <= (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::L)))
    {
      const AxisModifier& axis_modifier =
        m_half_axis_modifiers[index - (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::I))];
      value = apply_axis_modifier(value, axis_modifier);
      m_axis_state[sub_index] = get_scaled_value(value, axis_modifier);
    }
    else
    {
      m_axis_state[sub_index] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));
    }
  }
  else if (index < static_cast<u32>(Button::Count))
  {
    const u16 bit = u16(1) << s_button_indices[static_cast<u8>(index)];

    if (value >= 0.5f)
    {
      if (m_button_state & bit)
        System::SetRunaheadReplayFlag();

      m_button_state &= ~bit;
    }
    else
    {
      if (!(m_button_state & bit))
        System::SetRunaheadReplayFlag();

      m_button_state |= bit;
    }
  }
}

u32 NeGcon::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> NeGcon::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::L)] << 24 | m_axis_state[static_cast<size_t>(Axis::II)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::I)] << 8 | m_axis_state[static_cast<size_t>(Axis::Steering)];
}

void NeGcon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A23;

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
      m_transfer_state = TransferState::AnalogSteering;
      return true;
    }

    case TransferState::AnalogSteering:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::Steering)]);
      m_transfer_state = TransferState::AnalogI;
      return true;
    }

    case TransferState::AnalogI:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::I)]);
      m_transfer_state = TransferState::AnalogII;
      return true;
    }

    case TransferState::AnalogII:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::II)]);
      m_transfer_state = TransferState::AnalogL;
      return true;
    }

    case TransferState::AnalogL:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::L)]);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<NeGcon> NeGcon::Create(u32 index)
{
  return std::make_unique<NeGcon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb                      \
  }
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(NeGcon::Button::Count) + static_cast<u32>(halfaxis),               \
      InputBindingInfo::Type::HalfAxis, genb                                                                           \
  }

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGcon", "D-Pad Up"), ICON_PF_DPAD_UP, NeGcon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGcon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, NeGcon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGcon", "D-Pad Down"), ICON_PF_DPAD_DOWN, NeGcon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGcon", "D-Pad Left"), ICON_PF_DPAD_LEFT, NeGcon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGcon", "Start"), ICON_PF_START, NeGcon::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGcon", "A Button"), ICON_PF_BUTTON_A, NeGcon::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGcon", "B Button"), ICON_PF_BUTTON_B, NeGcon::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGcon", "I Button"), ICON_PF_BUTTON_ALT_1, NeGcon::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGcon", "II Button"), ICON_PF_BUTTON_ALT_2, NeGcon::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGcon", "Left Trigger"), ICON_PF_LEFT_TRIGGER_LT, NeGcon::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGcon", "Right Trigger"), ICON_PF_RIGHT_TRIGGER_RT, NeGcon::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Left"), ICON_PF_ANALOG_LEFT, NeGcon::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Right"), ICON_PF_ANALOG_RIGHT, NeGcon::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
// clang-format on

#undef AXIS
#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATE_NOOP("NeGcon", "Steering Axis Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for steering axis."), "0.00f", "0.00f", "0.99f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "SteeringSaturation", TRANSLATE_NOOP("NeGcon", "Steering Axis Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for steering axis."), "1.00f", "0.01f", "1.00f", "0.01f", "%.0f%%",
   nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringLinearity", TRANSLATE_NOOP("NeGcon", "Steering Axis Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for steering axis."), "0.00f", "-2.00f", "2.00f", "0.05f", "%.2f", nullptr,
   1.0f},
  {SettingInfo::Type::Float, "SteeringScaling", TRANSLATE_NOOP("NeGcon", "Steering Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for steering axis."), "1.00f", "0.01f", "10.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "IDeadzone", TRANSLATE_NOOP("NeGcon", "I Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button I."), "0.00f", "0.00f", "0.99f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "ISaturation", TRANSLATE_NOOP("NeGcon", "I Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button I."), "1.00f", "0.01f", "1.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "ILinearity", TRANSLATE_NOOP("NeGcon", "I Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button I."), "0.00f", "-2.00f", "2.00f", "0.01f", "%.2f", nullptr,
   1.0f},
  {SettingInfo::Type::Float, "IScaling", TRANSLATE_NOOP("NeGcon", "I Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button I."), "1.00f", "0.01f", "10.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "IIDeadzone", TRANSLATE_NOOP("NeGcon", "II Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button II."), "0.00f", "0.00f", "0.99f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "IISaturation", TRANSLATE_NOOP("NeGcon", "II Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button II."), "1.00f", "0.01f", "1.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "IILinearity", TRANSLATE_NOOP("NeGcon", "II Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button II."), "0.00f", "-2.00f", "2.00f", "0.01f", "%.2f", nullptr,
   1.0f},
  {SettingInfo::Type::Float, "IIScaling", TRANSLATE_NOOP("NeGcon", "II Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button II."), "1.00f", "0.01f", "10.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "LDeadzone", TRANSLATE_NOOP("NeGcon", "Left Trigger Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for left trigger."), "0.00f", "0.00f", "0.99f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "LSaturation", TRANSLATE_NOOP("NeGcon", "Left Trigger Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for left trigger."), "1.00f", "0.01f", "1.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::Float, "LLinearity", TRANSLATE_NOOP("NeGcon", "Left Trigger Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for left trigger."), "0.00f", "-2.00f", "2.00f", "0.01f", "%.2f", nullptr,
   1.0f},
  {SettingInfo::Type::Float, "LScaling", TRANSLATE_NOOP("NeGcon", "Left Trigger Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for left trigger."), "1.00f", "0.01f", "10.00f", "0.01f", "%.0f%%", nullptr,
   100.0f},
};

const Controller::ControllerInfo NeGcon::INFO = {
  ControllerType::NeGcon, "NeGcon",   TRANSLATE_NOOP("ControllerType", "NeGcon"),    ICON_PF_GAMEPAD,
  s_binding_info,         s_settings, Controller::VibrationCapabilities::NoVibration};

void NeGcon::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_steering_modifier = {
    .deadzone = si.GetFloatValue(section, "SteeringDeadzone", DEFAULT_STEERING_MODIFIER.deadzone),
    .saturation = si.GetFloatValue(section, "SteeringSaturation", DEFAULT_STEERING_MODIFIER.saturation),
    .linearity = si.GetFloatValue(section, "SteeringLinearity", DEFAULT_STEERING_MODIFIER.linearity),
    .scaling = si.GetFloatValue(section, "SteeringScaling", DEFAULT_STEERING_MODIFIER.scaling),
    .zero = DEFAULT_STEERING_MODIFIER.zero,
    .unit = DEFAULT_STEERING_MODIFIER.unit,
  };
  m_half_axis_modifiers[0] = {
    .deadzone = si.GetFloatValue(section, "IDeadzone", DEFAULT_PEDAL_MODIFIER.deadzone),
    .saturation = si.GetFloatValue(section, "ISaturation", DEFAULT_PEDAL_MODIFIER.saturation),
    .linearity = si.GetFloatValue(section, "ILinearity", DEFAULT_PEDAL_MODIFIER.linearity),
    .scaling = si.GetFloatValue(section, "IScaling", DEFAULT_PEDAL_MODIFIER.scaling),
    .zero = DEFAULT_PEDAL_MODIFIER.zero,
    .unit = DEFAULT_PEDAL_MODIFIER.unit,
  };
  m_half_axis_modifiers[1] = {
    .deadzone = si.GetFloatValue(section, "IIDeadzone", DEFAULT_PEDAL_MODIFIER.deadzone),
    .saturation = si.GetFloatValue(section, "IISaturation", DEFAULT_PEDAL_MODIFIER.saturation),
    .linearity = si.GetFloatValue(section, "IILinearity", DEFAULT_PEDAL_MODIFIER.linearity),
    .scaling = si.GetFloatValue(section, "IIScaling", DEFAULT_PEDAL_MODIFIER.scaling),
    .zero = DEFAULT_PEDAL_MODIFIER.zero,
    .unit = DEFAULT_PEDAL_MODIFIER.unit,
  };
  m_half_axis_modifiers[2] = {
    .deadzone = si.GetFloatValue(section, "LDeadzone", DEFAULT_PEDAL_MODIFIER.deadzone),
    .saturation = si.GetFloatValue(section, "LSaturation", DEFAULT_PEDAL_MODIFIER.saturation),
    .linearity = si.GetFloatValue(section, "LLinearity", DEFAULT_PEDAL_MODIFIER.linearity),
    .scaling = si.GetFloatValue(section, "LScaling", DEFAULT_PEDAL_MODIFIER.scaling),
    .zero = DEFAULT_PEDAL_MODIFIER.zero,
    .unit = DEFAULT_PEDAL_MODIFIER.unit,
  };
}
