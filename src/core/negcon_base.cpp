// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "negcon_base.h"
#include "system.h"

#include "common/settings_interface.h"

#include <algorithm>
#include <array>
#include <cmath>

// Mapping of Button to index of corresponding bit in m_button_state
static constexpr std::array<u8, static_cast<size_t>(NegConBase::Button::Count)> s_button_indices = {3, 4,  5,  6,
                                                                                                    7, 11, 12, 13};

static float ApplyAxisModifier(float value, const NegConBase::AxisModifier& axis_modifier);
static u8 GetScaledValue(float value, const NegConBase::AxisModifier& axis_modifier);

NegConBase::NegConBase(u32 index, u32 half_axis_bind_start)
  : Controller(index), m_half_axis_bind_start(half_axis_bind_start)
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
}

NegConBase::~NegConBase() = default;

float NegConBase::GetBindState(u32 index) const
{
  if (index == (m_half_axis_bind_start + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (m_half_axis_bind_start + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    float value = m_axis_state[static_cast<u32>(Axis::Steering)];
    value = value - 128.0f;
    value /= value < 0.0f ? 128.0f : 127.0f;
    value = std::clamp(value, -1.0f, 1.0f);
    if (index == (m_half_axis_bind_start + static_cast<u32>(HalfAxis::SteeringLeft)))
      value *= -1.0f;

    return std::max(0.0f, value);
  }
  else if (index >= m_half_axis_bind_start)
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (m_half_axis_bind_start + 1);
    if (sub_index >= m_axis_state.size())
      return 0.0f;

    return static_cast<float>(m_axis_state[sub_index]) * (1.0f / 255.0f);
  }
  else if (index < static_cast<u32>(Button::Count))
  {
    const u32 bit = s_button_indices[index];
    return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
  }
  else
  {
    return 0.0f;
  }
}

void NegConBase::SetBindState(u32 index, float value)
{
  // Steering Axis: -1..1 -> 0..255
  if (index == (m_half_axis_bind_start + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (m_half_axis_bind_start + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    value = ApplyAxisModifier(value, m_steering_modifier);

    m_half_axis_state[index - m_half_axis_bind_start] = std::clamp(value, 0.0f, 1.0f);

    const float merged = m_half_axis_state[1] - m_half_axis_state[0];
    m_axis_state[static_cast<u32>(Axis::Steering)] = GetScaledValue(merged, m_steering_modifier);
  }
  else if (index >= m_half_axis_bind_start)
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (m_half_axis_bind_start + 1);
    if (sub_index >= m_axis_state.size())
      return;

    if (index >= (m_half_axis_bind_start + static_cast<u32>(HalfAxis::I)) &&
        index <= (m_half_axis_bind_start + static_cast<u32>(HalfAxis::L)))
    {
      const AxisModifier& axis_modifier =
        m_half_axis_modifiers[index - (m_half_axis_bind_start + static_cast<u32>(HalfAxis::I))];
      value = ApplyAxisModifier(value, axis_modifier);
      m_axis_state[sub_index] = GetScaledValue(value, axis_modifier);
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
        System::SetRunaheadReplayFlag(false);

      m_button_state &= ~bit;
    }
    else
    {
      if (!(m_button_state & bit))
        System::SetRunaheadReplayFlag(false);

      m_button_state |= bit;
    }
  }
}

u32 NegConBase::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> NegConBase::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::L)] << 24 | m_axis_state[static_cast<size_t>(Axis::II)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::I)] << 8 | m_axis_state[static_cast<size_t>(Axis::Steering)];
}

void NegConBase::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_disable_socd = si.GetBoolValue(section, "DisableSOCD", false);
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

float ApplyAxisModifier(float value, const NegConBase::AxisModifier& axis_modifier)
{
  value = (value - axis_modifier.deadzone) / (axis_modifier.saturation - axis_modifier.deadzone);
  value = std::clamp(value, 0.0f, 1.0f);
  value = std::pow(value, std::exp(axis_modifier.linearity));
  return value;
}

u8 GetScaledValue(float value, const NegConBase::AxisModifier& axis_modifier)
{
  value = axis_modifier.scaling * axis_modifier.unit * value + axis_modifier.zero;
  return static_cast<u8>(std::clamp(std::round(value), 0.0f, 255.0f));
}
