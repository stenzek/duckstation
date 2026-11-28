// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "negcon.h"
#include "controller_helpers.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/settings_interface.h"

#include "IconsPromptFont.h"

// Mapping of Button to index of corresponding bit in m_button_state
static constexpr std::array<u8, static_cast<size_t>(NeGconInputs::Button::Count)> s_button_indices = {3, 4,  5,  6,
                                                                                                      7, 11, 12, 13};

static float ApplyAxisModifier(float value, const NeGconInputs::AxisModifier& axis_modifier);
static u8 GetScaledValue(float value, const NeGconInputs::AxisModifier& axis_modifier);

NeGconInputs::NeGconInputs(u32 half_axis_bind_start) : m_half_axis_bind_start(half_axis_bind_start)
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
}

float NeGconInputs::GetBindState(u32 index) const
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

void NeGconInputs::SetBindState(u32 index, float value)
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
    Controller::SetButtonPressed(m_button_state, u16(1) << s_button_indices[static_cast<u8>(index)], (value >= 0.5f));
  }
}

u32 NeGconInputs::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> NeGconInputs::GetAnalogInputBytes() const
{
  // Only steering is a stick axis. Report the other sticks as centred, so I/II/L don't read as directions (D7 codes).
  return 0x80808000u | m_axis_state[static_cast<u8>(Axis::Steering)];
}

void NeGconInputs::LoadSettings(const SettingsInterface& si, const char* section)
{
  static constexpr float DEFAULT_NEGCON_DEADZONE = 0.00f;
  static constexpr float DEFAULT_NEGCON_SATURATION = 1.00f;
  static constexpr float DEFAULT_NEGCON_LINEARITY = 0.00f;
  static constexpr float DEFAULT_NEGCON_SCALING = 1.00f;
  static constexpr float DEFAULT_NEGCON_STEERING_ZERO = 128.0f;
  static constexpr float DEFAULT_NEGCON_STEERING_UNIT = 128.0f;
  static constexpr float DEFAULT_NEGCON_PEDAL_ZERO = 0.0f;
  static constexpr float DEFAULT_NEGCON_PEDAL_UNIT = 255.0f;
  static constexpr const NeGconInputs::AxisModifier DEFAULT_STEERING_MODIFIER = {
    DEFAULT_NEGCON_DEADZONE, DEFAULT_NEGCON_SATURATION,    DEFAULT_NEGCON_LINEARITY,
    DEFAULT_NEGCON_SCALING,  DEFAULT_NEGCON_STEERING_ZERO, DEFAULT_NEGCON_STEERING_UNIT,
  };
  static constexpr const NeGconInputs::AxisModifier DEFAULT_PEDAL_MODIFIER = {
    DEFAULT_NEGCON_DEADZONE, DEFAULT_NEGCON_SATURATION, DEFAULT_NEGCON_LINEARITY,
    DEFAULT_NEGCON_SCALING,  DEFAULT_NEGCON_PEDAL_ZERO, DEFAULT_NEGCON_PEDAL_UNIT,
  };
  static constexpr auto load_modifier = [](const SettingsInterface& si, const char* section, const char* prefix,
                                           const AxisModifier& default_modifier) {
    return AxisModifier{
      si.GetFloatValue(section, TinyString::from_format("{}Deadzone", prefix), default_modifier.deadzone),
      si.GetFloatValue(section, TinyString::from_format("{}Saturation", prefix), default_modifier.saturation),
      si.GetFloatValue(section, TinyString::from_format("{}Linearity", prefix), default_modifier.linearity),
      si.GetFloatValue(section, TinyString::from_format("{}Scaling", prefix), default_modifier.scaling),
      default_modifier.zero,
      default_modifier.unit,
    };
  };

  m_disable_socd = si.GetBoolValue(section, "DisableSOCD", false);
  m_steering_modifier = load_modifier(si, section, "Steering", DEFAULT_STEERING_MODIFIER);
  m_half_axis_modifiers[0] = load_modifier(si, section, "I", DEFAULT_PEDAL_MODIFIER);
  m_half_axis_modifiers[1] = load_modifier(si, section, "II", DEFAULT_PEDAL_MODIFIER);
  m_half_axis_modifiers[2] = load_modifier(si, section, "L", DEFAULT_PEDAL_MODIFIER);
}

float ApplyAxisModifier(float value, const NeGconInputs::AxisModifier& axis_modifier)
{
  return std::pow(
    std::clamp((value - axis_modifier.deadzone) / (axis_modifier.saturation - axis_modifier.deadzone), 0.0f, 1.0f),
    std::exp(axis_modifier.linearity));
}

u8 GetScaledValue(float value, const NeGconInputs::AxisModifier& axis_modifier)
{
  return static_cast<u8>(
    std::clamp(std::round(axis_modifier.scaling * axis_modifier.unit * value + axis_modifier.zero), 0.0f, 255.0f));
}

NeGcon::NeGcon(u32 index) : Controller(index, ControllerType::NeGcon)
{
}

NeGcon::~NeGcon() = default;

void NeGcon::Reset()
{
  ResetTransferState();
}

bool NeGcon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || (apply_input_state && sw.GetVersion() >= 105))
  {
    sw.Do(&m_inputs.m_button_state);
    sw.Do(&m_inputs.m_axis_state);
  }
  else
  {
    sw.SkipBytes((sw.GetVersion() < 105) ? sizeof(m_inputs.m_button_state) :
                                           (sizeof(m_inputs.m_button_state) + sizeof(m_inputs.m_axis_state)));
  }

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

  return true;
}

float NeGcon::GetBindState(u32 index) const
{
  return m_inputs.GetBindState(index);
}

void NeGcon::SetBindState(u32 index, float value)
{
  m_inputs.SetBindState(index, value);
}

u32 NeGcon::GetButtonStateBits() const
{
  return m_inputs.GetButtonStateBits();
}

std::optional<u32> NeGcon::GetAnalogInputBytes() const
{
  return m_inputs.GetAnalogInputBytes();
}

void NeGcon::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_inputs.LoadSettings(si, section);
}

u8 NeGcon::GetIDByte() const
{
  return 0x23;
}

bool NeGcon::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  const u16 button_state = m_inputs.m_button_state;
  m_tx_buffer[2] =
    Truncate8(m_inputs.m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(button_state) : button_state);
  m_tx_buffer[3] = Truncate8(button_state >> 8);
  m_tx_buffer[4] = m_inputs.m_axis_state[static_cast<u8>(Axis::Steering)];
  m_tx_buffer[5] = m_inputs.m_axis_state[static_cast<u8>(Axis::I)];
  m_tx_buffer[6] = m_inputs.m_axis_state[static_cast<u8>(Axis::II)];
  m_tx_buffer[7] = m_inputs.m_axis_state[static_cast<u8>(Axis::L)];
  return true;
}

std::unique_ptr<NeGcon> NeGcon::Create(u32 index)
{
  return std::make_unique<NeGcon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, image_element, halfaxis, genb)                                             \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   image_element,                                                                                                      \
   static_cast<u32>(NeGcon::Button::Count) + static_cast<u32>(halfaxis),                                               \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGcon", "D-Pad Up"), ICON_PF_DPAD_UP, "Up", NeGcon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGcon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "Right", NeGcon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGcon", "D-Pad Down"), ICON_PF_DPAD_DOWN, "Down", NeGcon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGcon", "D-Pad Left"), ICON_PF_DPAD_LEFT, "Left", NeGcon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGcon", "Start"), ICON_PF_START, "Start", NeGcon::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGcon", "A Button"), ICON_PF_BUTTON_A, "A", NeGcon::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGcon", "B Button"), ICON_PF_BUTTON_B, "B", NeGcon::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGcon", "I Button"), ICON_PF_BUTTON_ALT_1, "I", NeGcon::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGcon", "II Button"), ICON_PF_BUTTON_ALT_2, "II", NeGcon::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGcon", "Left Trigger"), ICON_PF_LEFT_TRIGGER_LT, "L", NeGcon::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGcon", "Right Trigger"), ICON_PF_RIGHT_TRIGGER_RT, "R", NeGcon::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Left"), ICON_PF_ANALOG_LEFT, "Steering", NeGcon::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Right"), ICON_PF_ANALOG_RIGHT, "Steering", NeGcon::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
// clang-format on

#undef AXIS
#undef BUTTON
};

static constexpr SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "DisableSOCD",
   TRANSLATE_NOOP("NeGcon", "Disable Simultaneous Opposing Cardinal Directions"),
   TRANSLATE_NOOP("NeGcon", "Prevents concurrent left/right or up/down inputs from being presented to the game."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATE_NOOP("NeGcon", "Steering Axis Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for steering axis."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringSaturation", TRANSLATE_NOOP("NeGcon", "Steering Axis Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for steering axis."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringLinearity", TRANSLATE_NOOP("NeGcon", "Steering Axis Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for steering axis."), "0", "-2", "2", "0.05", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "SteeringScaling", TRANSLATE_NOOP("NeGcon", "Steering Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for steering axis."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IDeadzone", TRANSLATE_NOOP("NeGcon", "I Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button I."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ISaturation", TRANSLATE_NOOP("NeGcon", "I Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button I."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ILinearity", TRANSLATE_NOOP("NeGcon", "I Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button I."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "IScaling", TRANSLATE_NOOP("NeGcon", "I Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button I."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IIDeadzone", TRANSLATE_NOOP("NeGcon", "II Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button II."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IISaturation", TRANSLATE_NOOP("NeGcon", "II Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button II."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IILinearity", TRANSLATE_NOOP("NeGcon", "II Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button II."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "IIScaling", TRANSLATE_NOOP("NeGcon", "II Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button II."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LDeadzone", TRANSLATE_NOOP("NeGcon", "Left Trigger Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for left trigger."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LSaturation", TRANSLATE_NOOP("NeGcon", "Left Trigger Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for left trigger."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LLinearity", TRANSLATE_NOOP("NeGcon", "Left Trigger Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for left trigger."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "LScaling", TRANSLATE_NOOP("NeGcon", "Left Trigger Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for left trigger."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
};

const Controller::ControllerInfo NeGcon::INFO = {ControllerType::NeGcon,
                                                 "NeGcon",
                                                 TRANSLATE_NOOP("ControllerType", "NeGcon"),
                                                 ICON_PF_STEERING_WHEEL,
                                                 "images/controllers/negcon.svg",
                                                 s_binding_info,
                                                 s_settings};
