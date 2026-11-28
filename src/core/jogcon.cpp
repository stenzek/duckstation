// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "jogcon.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/log.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <algorithm>

LOG_CHANNEL(Controller);

JogCon::JogCon(u32 index) : ConfigModeController(index, ControllerType::JogCon)
{
}

JogCon::~JogCon() = default;

void JogCon::Reset()
{
  // Reset starts in jogcon mode?
  ResetConfigState();
  SetAnalogMode(true, false);
}

bool JogCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !ConfigModeController::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  s8 steering_state = m_steering_state;
  sw.Do(&button_state);
  sw.Do(&steering_state);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_steering_state = steering_state;
  }

  if (sw.GetVersion() >= 105)
  {
    sw.Do(&m_last_steering_state);
    sw.Do(&m_last_motor_command);
    sw.Do(&m_steering_hold_position);
    sw.Do(&m_steering_hold_strength);
    return !sw.HasError();
  }

  // The transfer restarts from idle, so the old transfer state is skipped.
  u8 unused_command = 0;
  u8 unused_step = 0;
  sw.Do(&unused_command);
  sw.Do(&unused_step);
  sw.Do(&m_status_byte);
  sw.Do(&m_last_steering_state);
  sw.Do(&m_last_motor_command);
  sw.Do(&m_steering_hold_position);
  sw.Do(&m_steering_hold_strength);

  sw.Do(&m_configuration_mode);

  bool jogcon_mode = m_analog_mode;
  sw.Do(&jogcon_mode);

  std::array<u8, MAX_RESPONSE_LENGTH> unused_buffer;
  sw.Do(&unused_buffer);
  sw.Do(&unused_buffer);
  sw.Do(&m_rumble_config);

  // Only the first five motor bytes could be mapped, the motor worked without DualShock mode, and there was no lock.
  if (sw.IsReading())
  {
    m_rumble_config[5] = 0xFF;
    m_dualshock_enabled =
      m_configuration_mode || (std::find(m_rumble_config.begin(), m_rumble_config.end(), 0) != m_rumble_config.end());
    m_analog_locked = false;
    // The old force wasn't saved, so the motor stops, and a hold is applied by the next poll.
    m_motor_state[0] =
      (m_steering_hold_strength != 0) ? static_cast<u8>((MOTOR_COMMAND_HOLD << 4) | m_steering_hold_strength) : 0;
    SetMotorDirection(MOTOR_COMMAND_STOP, 0);

    Controller::ResetTransferState();
    FinishLoadState(jogcon_mode, m_motor_state);
  }

  return !sw.HasError();
}

float JogCon::GetBindState(u32 index) const
{
  if (index >= LED_BIND_START_INDEX)
    return BoolToFloat(index == LED_BIND_START_INDEX && m_analog_mode);
  else if (index >= MOTOR_BIND_START_INDEX)
    return m_last_strength;
  else if (index >= HALFAXIS_BIND_START_INDEX)
    return static_cast<float>(m_half_axis_state[index - HALFAXIS_BIND_START_INDEX]) * (1.0f / 255.0f);
  else if (index < static_cast<u32>(Button::Mode))
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  else
    return 0.0f;
}

void JogCon::SetBindState(u32 index, float value)
{
  if (index == static_cast<u32>(Button::Mode))
  {
    // analog toggle
    if (value >= m_button_deadzone)
      ToggleAnalogMode();

    return;
  }
  else if (index >= static_cast<u32>(Button::MaxCount))
  {
    const u32 sub_index = index - static_cast<u32>(Button::MaxCount);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return;

    const u8 u8_value = static_cast<u8>(
      std::clamp(((value < m_analog_deadzone) ? 0.0f : value) * m_analog_sensitivity * 255.0f, 0.0f, 255.0f));
    if (u8_value == m_half_axis_state[sub_index])
      return;

    m_half_axis_state[sub_index] = u8_value;

    const s8 prev_steering_state = m_steering_state;

    m_steering_state =
      (m_half_axis_state[static_cast<u32>(HalfAxis::SteeringRight)] != 0) ?
        static_cast<s8>((m_half_axis_state[static_cast<u32>(HalfAxis::SteeringRight)] / 2)) :
        -static_cast<s8>((static_cast<u32>(m_half_axis_state[static_cast<u32>(HalfAxis::SteeringLeft)]) + 1) / 2);

    if (m_steering_state != prev_steering_state)
      System::SetRunaheadReplayFlag(true);
  }

  SetButtonPressed(m_button_state, u16(1) << static_cast<u8>(index), (value >= m_button_deadzone));
}

u32 JogCon::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

u8 JogCon::GetAnalogModeID() const
{
  return 0xE;
}

u8 JogCon::GetNumMotors() const
{
  return 1;
}

u8 JogCon::GetCommand45Actuators() const
{
  return 0x01;
}

void JogCon::PatchCommand4C(u8 param)
{
  if (param == 0x00)
    m_tx_buffer[5] = 0x04;
  else if (param == 0x01)
    m_tx_buffer[4] = 0x03;
}

bool JogCon::HasLegacyRumble() const
{
  return false;
}

bool JogCon::CanEnterConfigMode() const
{
  // Config mode can't be entered from digital mode.
  return m_analog_mode;
}

u32 JogCon::GetMotorBindIndex(u32 motor) const
{
  return MOTOR_BIND_START_INDEX;
}

void JogCon::SetMotorState(u32 motor, u8 value)
{
  const u8 command = (value >> 4);
  const u8 strength = (value & 0x0F);

  DEV_LOG("0x{:02X} command=0x{:X} force={}", value, command, strength);

  switch (command)
  {
    case MOTOR_COMMAND_STOP:
    {
      m_motor_state[motor] = 0;
      m_steering_hold_strength = 0;
      SetMotorDirection(MOTOR_COMMAND_STOP, 0);
    }
    break;

    case MOTOR_COMMAND_RIGHT:
    case MOTOR_COMMAND_LEFT:
    {
      m_motor_state[motor] = value;
      m_steering_hold_strength = 0;
      SetMotorDirection(command, strength);
    }
    break;

    case MOTOR_COMMAND_HOLD:
    case MOTOR_COMMAND_DROP_REVOLUTIONS_AND_HOLD:
    {
      DEV_LOG("Hold wheel in position {} with {} strength.", m_steering_hold_position, strength);
      // The hold force is updated by the next poll after loading a state.
      m_motor_state[motor] = (strength != 0) ? static_cast<u8>((MOTOR_COMMAND_HOLD << 4) | strength) : 0;
      m_steering_hold_strength = strength;
      if (strength != 0)
        UpdateSteeringHold();
      else
        SetMotorDirection(MOTOR_COMMAND_STOP, 0);

      if (command == MOTOR_COMMAND_DROP_REVOLUTIONS_AND_HOLD)
        ERROR_LOG("JogCon Drop revolutions and hold command is not handled.");
    }
    break;

    case MOTOR_COMMAND_DROP_REVOLUTIONS:
    {
      ERROR_LOG("JogCon drop revolutions command is not handled.");
    }
    break;

    case MOTOR_COMMAND_NEW_HOLD:
    {
      ERROR_LOG("JogCon new hold position {}", m_steering_state);
      m_steering_hold_position = m_steering_state;
    }
    break;

    default:
    {
      ERROR_LOG("Unknown JogCon command 0x{:X}", command);
    }
    break;
  }

  m_last_motor_command = command;
}

void JogCon::SetMotorDirection(u8 direction_command, u8 strength)
{
  if (direction_command == MOTOR_COMMAND_STOP || strength == 0)
  {
    DEV_LOG("Stop motor");
    if (m_force_feedback_device)
      m_force_feedback_device->DisableForce(ForceFeedbackDevice::Effect::Constant);

    if (m_last_strength != 0.0f)
    {
      m_last_strength = 0.0f;
      InputManager::SetPadVibrationIntensity(m_index, MOTOR_BIND_START_INDEX, 0.0f);
    }

    return;
  }

  DEV_LOG("Turn wheel {} with {} strength", (direction_command == MOTOR_COMMAND_LEFT) ? "LEFT" : "RIGHT", strength);

  const float f_strength = (static_cast<float>(strength) / 15.0f);
  if (m_force_feedback_device)
  {
    // 0->15 => -32768..32767, direction is flipped because it's indicating where the force is coming _from_.
    const s32 ffb_value =
      static_cast<s32>(f_strength * ((direction_command == MOTOR_COMMAND_LEFT) ? 32767.0f : -32768.0f));
    m_force_feedback_device->SetConstantForce(ffb_value);
  }

  if (f_strength != m_last_strength)
  {
    m_last_strength = f_strength;
    InputManager::SetPadVibrationIntensity(m_index, MOTOR_BIND_START_INDEX, f_strength);
  }
}

void JogCon::UpdateSteeringHold()
{
  if (m_steering_hold_strength > 0)
  {
    const u8 direction_command =
      (std::abs(static_cast<int>(m_steering_state) - static_cast<int>(m_steering_hold_position)) <
       m_steering_hold_deadzone) ?
        MOTOR_COMMAND_STOP :
        ((m_steering_state < m_steering_hold_position) ? MOTOR_COMMAND_RIGHT : MOTOR_COMMAND_LEFT);
    DEV_LOG("Hold strength {} pos {} hold {} dir {}", m_steering_hold_strength, m_steering_state,
            m_steering_hold_position, direction_command);
    SetMotorDirection(direction_command, m_steering_hold_strength);
  }
}

void JogCon::Poll()
{
  m_tx_buffer[2] = Truncate8(m_button_state);
  m_tx_buffer[3] = Truncate8(m_button_state >> 8);

  m_tx_buffer[4] = Truncate8(m_steering_state);
  m_tx_buffer[5] = Truncate8(m_steering_state >> 8); // 0xFF if negative, otherwise 0x00

  u8 rotation_state = 0;
  if (m_steering_state > m_last_steering_state)
    rotation_state = 1;
  else if (m_steering_state < m_last_steering_state)
    rotation_state = 2;

  m_tx_buffer[6] = rotation_state | (m_last_motor_command << 4);

  m_last_steering_state = m_steering_state;
  UpdateSteeringHold();
}

void JogCon::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_button_deadzone = std::clamp(si.GetFloatValue(section, "ButtonDeadzone", DEFAULT_BUTTON_DEADZONE), 0.01f, 1.0f);
  m_steering_hold_deadzone = static_cast<s8>(std::ceil(
    std::clamp(si.GetFloatValue(section, "SteeringHoldDeadzone", DEFAULT_STEERING_HOLD_DEADZONE), 0.0f, 1.0f) *
    127.0f));

  std::string force_feedback_device_name = si.GetStringValue(section, "ForceFeedbackDevice");
  if (m_force_feedback_device_name != force_feedback_device_name)
  {
    m_force_feedback_device_name = std::move(force_feedback_device_name);
    m_force_feedback_device.reset();
    if (!m_force_feedback_device_name.empty())
    {
      Error error;
      m_force_feedback_device = InputManager::CreateForceFeedbackDevice(m_force_feedback_device_name, &error);
      if (!m_force_feedback_device)
      {
        ERROR_LOG("Failed to create force feedback device: {}", error.GetDescription());
        if (initial)
        {
          Host::AddIconOSDMessage(
            OSDMessageType::Warning, fmt::format("NoFFDevice{}", m_index), ICON_EMOJI_WARNING,
            fmt::format(TRANSLATE_FS("JogCon", "Failed to create force feedback device for Port {}:\n{}"),
                        Controller::GetPortDisplayName(m_index, g_settings.multitap_mode), error.GetDescription()));
        }
      }
    }
  }
}

std::unique_ptr<JogCon> JogCon::Create(u32 index)
{
  return std::make_unique<JogCon>(index);
}

constinit const Controller::ControllerBindingInfo JogCon::s_binding_info[] = {
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

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("JogCon", "D-Pad Up"), ICON_PF_DPAD_UP, "Up", JogCon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("JogCon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "Right", JogCon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("JogCon", "D-Pad Down"), ICON_PF_DPAD_DOWN, "Down", JogCon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("JogCon", "D-Pad Left"), ICON_PF_DPAD_LEFT, "Left", JogCon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("JogCon", "Triangle"), ICON_PF_BUTTON_TRIANGLE, "Triangle", JogCon::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("JogCon", "Circle"), ICON_PF_BUTTON_CIRCLE, "Circle", JogCon::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("JogCon", "Cross"), ICON_PF_BUTTON_CROSS, "Cross", JogCon::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("JogCon", "Square"), ICON_PF_BUTTON_SQUARE, "Square", JogCon::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("JogCon", "Select"), ICON_PF_SELECT_SHARE, "Select", JogCon::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("JogCon", "Start"), ICON_PF_START, "Start", JogCon::Button::Start, GenericInputBinding::Start),
  BUTTON("L1", TRANSLATE_NOOP("JogCon", "L1"), ICON_PF_LEFT_SHOULDER_L1, "L1", JogCon::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("JogCon", "R1"), ICON_PF_RIGHT_SHOULDER_R1, "R1", JogCon::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("JogCon", "L2"), ICON_PF_LEFT_TRIGGER_L2, "L2", JogCon::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("JogCon", "R2"), ICON_PF_RIGHT_TRIGGER_R2, "R2", JogCon::Button::R2, GenericInputBinding::R2),
  BUTTON("Mode", TRANSLATE_NOOP("JogCon", "Mode"), ICON_PF_ANALOG_LEFT_RIGHT, "Mode", JogCon::Button::Mode, GenericInputBinding::System),

  AXIS("SteeringLeft", TRANSLATE_NOOP("JogCon", "Steering Left"), ICON_PF_ANALOG_LEFT, "Steering", JogCon::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("JogCon", "Steering Right"), ICON_PF_ANALOG_RIGHT, "Steering", JogCon::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),

  // clang-format on

  {"ModeLED", TRANSLATE_NOOP("JogCon", "Mode LED"), ICON_PF_LED, "ModeLED", LED_BIND_START_INDEX,
   InputBindingInfo::Type::LED, GenericInputBinding::ModeLED},

  {"Motor", TRANSLATE_NOOP("JogCon", "Vibration Motor"), ICON_PF_VIBRATION, nullptr, MOTOR_BIND_START_INDEX,
   InputBindingInfo::Type::Motor, GenericInputBinding::LargeMotor},

  {"ForceFeedbackDevice", TRANSLATE_NOOP("JogCon", "Force Feedback Device"), nullptr, nullptr,
   FFDEVICE_BIND_START_INDEX, InputBindingInfo::Type::Device, GenericInputBinding::Unknown},

#undef BUTTON
#undef AXIS
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("JogCon", "Analog Deadzone"),
   TRANSLATE_NOOP("JogCon",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0", "0", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("JogCon", "Analog Sensitivity"),
   TRANSLATE_NOOP("JogCon", "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended "
                            "when using recent controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33", "0.01", "2", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ButtonDeadzone", TRANSLATE_NOOP("JogCon", "Button/Trigger Deadzone"),
   TRANSLATE_NOOP(
     "JogCon",
     "Sets the deadzone for activating buttons/triggers, i.e. the fraction of the trigger which will be ignored."),
   "0.25", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringHoldDeadzone", TRANSLATE_NOOP("JogCon", "Steering Hold Deadzone"),
   TRANSLATE_NOOP(
     "JogCon", "Sets the deadzone for holding the wheel at the set position, i.e. when it will not trigger an effect."),
   "0.03", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
};

const Controller::ControllerInfo JogCon::INFO = {ControllerType::JogCon,
                                                 "JogCon",
                                                 TRANSLATE_NOOP("ControllerType", "JogCon"),
                                                 ICON_PF_JOGCON_CONTROLLER,
                                                 "images/controllers/jogcon.svg",
                                                 s_binding_info,
                                                 s_settings};
