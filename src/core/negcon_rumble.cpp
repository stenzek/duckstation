// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "negcon_rumble.h"
#include "controller_helpers.h"
#include "host.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <algorithm>
#include <cmath>

LOG_CHANNEL(Controller);

NeGconRumble::NeGconRumble(u32 index) : ConfigModeController(index, ControllerType::NeGconRumble)
{
}

NeGconRumble::~NeGconRumble() = default;

void NeGconRumble::Reset()
{
  ResetConfigState();

  // NOTE: Should be NeGconRumble, but we don't want to break it for games that haven't opted in.
  if (CanStartInAnalogMode(ControllerType::AnalogController))
    SetAnalogMode(true, false);
}

bool NeGconRumble::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() < 105) [[unlikely]]
    return DoLegacyState(sw);

  if (!ConfigModeController::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || apply_input_state)
  {
    sw.Do(&m_inputs.m_button_state);
    sw.DoBytes(m_inputs.m_axis_state.data(), m_inputs.m_axis_state.size());
  }
  else
  {
    sw.SkipBytes(sizeof(m_inputs.m_button_state) + sizeof(m_inputs.m_axis_state));
  }

  return !sw.HasError();
}

bool NeGconRumble::DoLegacyState(StateWrapper& sw)
{
  // The transfer restarts from idle, so the old transfer state is skipped.
  bool analog_mode = m_analog_mode;
  sw.Do(&analog_mode);
  sw.Do(&m_dualshock_enabled);
  sw.Do(&m_configuration_mode);
  sw.DoEx(&m_status_byte, 55, static_cast<u8>(0x5A));

  // we're never applying input state from legacy states
  u16 unused_button_state = m_inputs.m_button_state;
  sw.DoEx(&unused_button_state, 44, static_cast<u16>(0xFFFF));

  sw.SkipBytes(sizeof(u8)); // unused_command
  sw.DoEx(&m_rumble_config, 45, {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu});
  if (sw.GetVersion() >= 45)
    sw.SkipBytes(sizeof(int) + sizeof(int)); // rumble_config_large_motor_index, rumble_config_small_motor_index
  sw.DoEx(&m_analog_toggle_queued, 45, false);

  // The large motor was first.
  MotorState motor_state;
  sw.Do(&motor_state);
  std::swap(motor_state[SmallMotor], motor_state[LargeMotor]);

  if (sw.IsReading())
  {
    Controller::ResetTransferState();
    FinishLoadState(analog_mode, motor_state);
  }

  return !sw.HasError();
}

float NeGconRumble::GetBindState(u32 index) const
{
  if (index >= LED_BIND_START_INDEX)
    return BoolToFloat(index == LED_BIND_START_INDEX && m_analog_mode);
  else if (index >= MOTOR_BIND_START_INDEX)
    return GetMotorStrength((index == GetMotorBindIndex(LargeMotor)) ? LargeMotor : SmallMotor);
  else
    return m_inputs.GetBindState(index);
}

void NeGconRumble::SetBindState(u32 index, float value)
{
  if (index == static_cast<s32>(Button::Analog))
  {
    // analog toggle
    if (value >= 0.5f)
      ToggleAnalogMode();

    return;
  }

  m_inputs.SetBindState(index, value);
}

u32 NeGconRumble::GetButtonStateBits() const
{
  return m_inputs.GetButtonStateBits();
}

std::optional<u32> NeGconRumble::GetAnalogInputBytes() const
{
  return m_inputs.GetAnalogInputBytes();
}

u8 NeGconRumble::GetAnalogModeID() const
{
  return 0x2;
}

void NeGconRumble::PatchCommand4C(u8 param)
{
  if (param == 0x00)
    m_tx_buffer[5] = 0x04;
  else if (param == 0x01)
    m_tx_buffer[5] = 0x02;
}

u32 NeGconRumble::GetMotorBindIndex(u32 motor) const
{
  // The large motor is bound first.
  return MOTOR_BIND_START_INDEX + ((motor == LargeMotor) ? 0 : 1);
}

void NeGconRumble::Poll()
{
  const u16 button_state = m_inputs.m_button_state;
  m_tx_buffer[2] =
    Truncate8(m_inputs.m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(button_state) : button_state);
  m_tx_buffer[3] = Truncate8(button_state >> 8);

  if (m_configuration_mode || m_analog_mode)
  {
    m_tx_buffer[4] = m_inputs.m_axis_state[static_cast<u8>(Axis::Steering)];
    m_tx_buffer[5] = m_inputs.m_axis_state[static_cast<u8>(Axis::I)];
    m_tx_buffer[6] = m_inputs.m_axis_state[static_cast<u8>(Axis::II)];
    m_tx_buffer[7] = m_inputs.m_axis_state[static_cast<u8>(Axis::L)];
  }
}

std::unique_ptr<NeGconRumble> NeGconRumble::Create(u32 index)
{
  return std::make_unique<NeGconRumble>(index);
}

constinit const Controller::ControllerBindingInfo NeGconRumble::s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, image_element, halfaxis, genb)                                             \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   image_element,                                                                                                      \
   HALFAXIS_BIND_START_INDEX + static_cast<u32>(halfaxis),                                                             \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}
#define MOTOR(name, display_name, icon_name, index, genb)                                                              \
  {name, display_name, icon_name, nullptr, MOTOR_BIND_START_INDEX + index, InputBindingInfo::Type::Motor, genb}
#define MODE_LED(name, display_name, icon_name, index, genb)                                                           \
  {name, display_name, icon_name, nullptr, LED_BIND_START_INDEX + index, InputBindingInfo::Type::LED, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGcon", "D-Pad Up"), ICON_PF_DPAD_UP, "Up", NeGconRumble::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGcon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "Right", NeGconRumble::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGcon", "D-Pad Down"), ICON_PF_DPAD_DOWN, "Down", NeGconRumble::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGcon", "D-Pad Left"), ICON_PF_DPAD_LEFT, "Left", NeGconRumble::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGcon", "Start"),ICON_PF_START, "Start", NeGconRumble::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGcon", "A Button"), ICON_PF_BUTTON_A, "A", NeGconRumble::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGcon", "B Button"), ICON_PF_BUTTON_B, "B", NeGconRumble::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGcon", "I Button"), ICON_PF_BUTTON_ALT_1, "I", NeGconRumble::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGcon", "II Button"), ICON_PF_BUTTON_ALT_2, "II", NeGconRumble::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGcon", "Left Trigger"), ICON_PF_LEFT_TRIGGER_LT, "L", NeGconRumble::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGcon", "Right Trigger"), ICON_PF_RIGHT_TRIGGER_RT, "R", NeGconRumble::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Left"), ICON_PF_ANALOG_LEFT, "Steering", NeGconRumble::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Right"), ICON_PF_ANALOG_RIGHT, "Steering", NeGconRumble::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
  BUTTON("Analog", TRANSLATE_NOOP("NeGcon", "Analog Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, nullptr, NeGconRumble::Button::Analog, GenericInputBinding::System),
  
  MOTOR("LargeMotor", TRANSLATE_NOOP("NeGcon", "Large Motor"), ICON_PF_VIBRATION_L, 0, GenericInputBinding::LargeMotor),
  MOTOR("SmallMotor", TRANSLATE_NOOP("NeGcon", "Small Motor"), ICON_PF_VIBRATION, 1, GenericInputBinding::SmallMotor),

  MODE_LED("ModeLED", TRANSLATE_NOOP("NeGcon", "Mode LED"), ICON_PF_LED, 0, GenericInputBinding::ModeLED),
// clang-format on

#undef MOTOR
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
  {SettingInfo::Type::Integer, "LargeMotorVibrationBias", TRANSLATE_NOOP("NeGcon", "Large Motor Vibration Bias"),
   TRANSLATE_NOOP("NeGcon",
                  "Sets the bias value for the large vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::Integer, "SmallMotorVibrationBias", TRANSLATE_NOOP("NeGcon", "Small Motor Vibration Bias"),
   TRANSLATE_NOOP("NeGcon",
                  "Sets the bias value for the small vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
};

const Controller::ControllerInfo NeGconRumble::INFO = {ControllerType::NeGconRumble,
                                                       "NeGconRumble",
                                                       TRANSLATE_NOOP("ControllerType", "NeGcon (Rumble)"),
                                                       ICON_PF_STEERING_WHEEL,
                                                       "images/controllers/negcon.svg",
                                                       s_binding_info,
                                                       s_settings};

void NeGconRumble::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_inputs.LoadSettings(si, section);
  m_vibration_bias[LargeMotor] =
    static_cast<s16>(std::clamp(si.GetIntValue(section, "LargeMotorVibrationBias", DEFAULT_VIBRATION_BIAS), -255, 255));
  m_vibration_bias[SmallMotor] =
    static_cast<s16>(std::clamp(si.GetIntValue(section, "SmallMotorVibrationBias", DEFAULT_VIBRATION_BIAS), -255, 255));
}
