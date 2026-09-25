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

NeGconRumble::NeGconRumble(u32 index) : NegConBase(index, HALFAXIS_BIND_START_INDEX)
{
  m_status_byte = 0x5A;
  m_rumble_config.fill(0xFF);
}

NeGconRumble::~NeGconRumble() = default;

ControllerType NeGconRumble::GetType() const
{
  return ControllerType::NeGconRumble;
}

void NeGconRumble::Reset()
{
  m_command = Command::Idle;
  m_command_step = 0;
  m_rx_buffer.fill(0x00);
  m_tx_buffer.fill(0x00);
  m_analog_mode = false;
  m_configuration_mode = false;

  for (u32 i = 0; i < NUM_MOTORS; i++)
  {
    if (m_motor_state[i] != 0)
      SetMotorState(i, 0);
  }

  m_dualshock_enabled = false;
  ResetRumbleConfig();

  m_status_byte = 0x5A;

  // NOTE: Should be NeGconRumble, but we don't want to break it for games that haven't opted in.
  if (CanStartInAnalogMode(ControllerType::AnalogController))
    SetAnalogMode(true, false);
}

bool NeGconRumble::DoState(StateWrapper& sw, bool apply_input_state)
{
  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);
  sw.Do(&m_dualshock_enabled);
  sw.Do(&m_configuration_mode);
  sw.DoEx(&m_status_byte, 55, static_cast<u8>(0x5A));

  u16 button_state = m_button_state;
  sw.DoEx(&button_state, 44, static_cast<u16>(0xFFFF));
  if (apply_input_state)
    m_button_state = button_state;
  else
    m_analog_mode = old_analog_mode;

  sw.Do(&m_command);

  sw.DoEx(&m_rumble_config, 45, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  sw.DoEx(&m_rumble_config_large_motor_index, 45, -1);
  sw.DoEx(&m_rumble_config_small_motor_index, 45, -1);
  sw.DoEx(&m_analog_toggle_queued, 45, false);

  MotorState motor_state = m_motor_state;
  sw.Do(&motor_state);

  if (sw.IsReading())
  {
    for (u8 i = 0; i < NUM_MOTORS; i++)
      SetMotorState(i, motor_state[i]);

    if (old_analog_mode != m_analog_mode)
    {
      Host::AddIconOSDMessage(OSDMessageType::Quick, fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
                              fmt::format(m_analog_mode ?
                                            TRANSLATE_FS("Controller", "Controller {} switched to analog mode.") :
                                            TRANSLATE_FS("Controller", "Controller {} switched to digital mode."),
                                          m_index + 1u));
    }
  }
  return true;
}

float NeGconRumble::GetBindState(u32 index) const
{
  if (index >= LED_BIND_START_INDEX)
  {
    return BoolToFloat(index == LED_BIND_START_INDEX && m_analog_mode);
  }
  else if (index >= MOTOR_BIND_START_INDEX)
  {
    return GetMotorStrength(index - MOTOR_BIND_START_INDEX);
  }
  else
  {
    return NegConBase::GetBindState(index);
  }
}

void NeGconRumble::SetBindState(u32 index, float value)
{
  if (index == static_cast<s32>(Button::Analog))
  {
    // analog toggle
    if (value >= 0.5f)
    {
      if (m_command == Command::Idle)
        ProcessAnalogModeToggle();
      else
        m_analog_toggle_queued = true;
    }

    return;
  }
  NegConBase::SetBindState(index, value);
}

void NeGconRumble::ResetTransferState()
{
  if (m_analog_toggle_queued)
  {
    ProcessAnalogModeToggle();
    m_analog_toggle_queued = false;
  }

  m_command = Command::Idle;
  m_command_step = 0;
}

void NeGconRumble::SetAnalogMode(bool enabled, bool show_message)
{
  if (m_analog_mode == enabled)
    return;

  InputManager::SetPadLEDState(m_index, BoolToFloat(enabled));

  INFO_LOG("Controller {} switched to {} mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  if (show_message)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Quick, fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
      enabled ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
                fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
  }

  m_analog_mode = enabled;
}

void NeGconRumble::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Quick, fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
      fmt::format(m_analog_mode ?
                    TRANSLATE_FS("Controller", "Controller {} is locked to analog mode by the game.") :
                    TRANSLATE_FS("Controller", "Controller {} is locked to digital mode by the game."),
                  m_index + 1u));
  }
  else
  {
    SetAnalogMode(!m_analog_mode, true);
    ResetRumbleConfig();

    if (m_dualshock_enabled)
      m_status_byte = 0x00;
  }
}

void NeGconRumble::SetMotorState(u32 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  if (m_motor_state[motor] != value)
  {
    m_motor_state[motor] = value;

    const float hvalue = GetMotorStrength(motor);
    DEV_LOG("Set {} motor to {} (raw {})", (motor == LargeMotor) ? "large" : "small", hvalue, m_motor_state[motor]);
    InputManager::SetPadVibrationIntensity(m_index, MOTOR_BIND_START_INDEX + motor, hvalue);
  }
}

float NeGconRumble::GetMotorStrength(u32 motor) const
{
  // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
  const u8 state = m_motor_state[motor];
  const double x = static_cast<double>(std::clamp<s32>(static_cast<s32>(state) + m_vibration_bias[motor], 0, 255));
  const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                          156.82454281087692 * x + 3.637978807091713e-11;

  return (state != 0) ? static_cast<float>(strength / 65535.0) : 0.0f;
}

void NeGconRumble::ResetRumbleConfig()
{
  m_rumble_config.fill(0xFF);

  m_rumble_config_large_motor_index = -1;
  m_rumble_config_small_motor_index = -1;

  SetMotorState(LargeMotor, 0);
  SetMotorState(SmallMotor, 0);
}

void NeGconRumble::SetMotorStateForConfigIndex(int index, u8 value)
{
  if (m_rumble_config_small_motor_index == index)
    SetMotorState(SmallMotor, ((value & 0x01) != 0) ? 255 : 0);
  else if (m_rumble_config_large_motor_index == index)
    SetMotorState(LargeMotor, value);
}

u8 NeGconRumble::GetResponseNumHalfwords() const
{
  if (m_configuration_mode || m_analog_mode)
    return 0x3;

  return (0x1);
}

u8 NeGconRumble::GetModeID() const
{
  if (m_configuration_mode)
    return 0xF;

  if (m_analog_mode)
    return 0x2;

  return 0x4;
}

u8 NeGconRumble::GetIDByte() const
{
  return Truncate8((GetModeID() << 4) | GetResponseNumHalfwords());
}

bool NeGconRumble::Transfer(const u8 data_in, u8* data_out)
{
  bool ack;
  m_rx_buffer[m_command_step] = data_in;

  switch (m_command)
  {
    case Command::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        DEBUG_LOG("ACK controller access");
        m_command = Command::Ready;
        return true;
      }

      DEV_LOG("Unknown data_in = 0x{:02X}", data_in);
      return false;
    }
    break;

    case Command::Ready:
    {
      if (data_in == 0x42)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::ReadPad;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      }
      else if (data_in == 0x43)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::ConfigModeSetMode;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      }
      else if (m_configuration_mode && data_in == 0x44)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::SetAnalogMode;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        ResetRumbleConfig();
      }
      else if (m_configuration_mode && data_in == 0x45)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::GetAnalogMode;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x01, 0x02, BoolToUInt8(m_analog_mode), 0x02, 0x01, 0x00};
      }
      else if (m_configuration_mode && data_in == 0x46)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::Command46;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      }
      else if (m_configuration_mode && data_in == 0x47)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::Command47;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00};
      }
      else if (m_configuration_mode && data_in == 0x4C)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::Command4C;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      }
      else if (m_configuration_mode && data_in == 0x4D)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::GetSetRumble;
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        m_rumble_config_large_motor_index = -1;
        m_rumble_config_small_motor_index = -1;
      }
      else
      {
        if (m_configuration_mode)
          ERROR_LOG("Unimplemented config mode command 0x{:02X}", data_in);

        *data_out = 0xFF;
        return false;
      }
    }
    break;

    case Command::ReadPad:
    {
      const int rumble_index = m_command_step - 2;

      switch (m_command_step)
      {
        case 2:
        {
          m_tx_buffer[m_command_step] =
            Truncate8(m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(m_button_state) : m_button_state);

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 3:
        {
          m_tx_buffer[m_command_step] = Truncate8(m_button_state >> 8);

          if (m_dualshock_enabled)
          {
            SetMotorStateForConfigIndex(rumble_index, data_in);
          }
          else
          {
            bool legacy_rumble_on = (m_rx_buffer[2] & 0xC0) == 0x40 && (m_rx_buffer[3] & 0x01) != 0;
            SetMotorState(SmallMotor, legacy_rumble_on ? 255 : 0);
          }
        }
        break;

        case 4:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::Steering)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 5:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::I)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 6:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::II)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 7:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::L)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        default:
        {
        }
        break;
      }
    }
    break;

    case Command::ConfigModeSetMode:
    {
      if (!m_configuration_mode)
      {
        switch (m_command_step)
        {
          case 2:
          {
            m_tx_buffer[m_command_step] =
              Truncate8(m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(m_button_state) : m_button_state);
          }
          break;

          case 3:
          {
            m_tx_buffer[m_command_step] = Truncate8(m_button_state >> 8);
          }
          break;

          case 4:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::Steering)];
          }
          break;

          case 5:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::I)];
          }
          break;

          case 6:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::II)];
          }
          break;

          case 7:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::L)];
          }
          break;

          default:
          {
          }
          break;
        }
      }

      if (m_command_step == (static_cast<s32>(m_response_length) - 1))
      {
        m_configuration_mode = (m_rx_buffer[2] == 1);

        if (m_configuration_mode)
        {
          m_dualshock_enabled = true;
          m_status_byte = 0x5A;
        }

        DEV_LOG("0x{:02x}({}) config mode", m_rx_buffer[2], m_configuration_mode ? "enter" : "leave");
      }
    }
    break;

    case Command::SetAnalogMode:
    {
      if (m_command_step == 2)
      {
        DEV_LOG("analog mode val 0x{:02x}", data_in);

        if (data_in == 0x00 || data_in == 0x01)
          SetAnalogMode((data_in == 0x01), true);
      }
      else if (m_command_step == 3)
      {
        DEV_LOG("analog mode lock 0x{:02x}", data_in);

        if (data_in == 0x02 || data_in == 0x03)
          m_analog_locked = (data_in == 0x03);
      }
    }
    break;

    case Command::GetAnalogMode:
    {
      // Intentionally empty, analog mode byte is set in reply buffer when command is first received
    }
    break;

    case Command::Command46:
    {
      if (m_command_step == 2)
      {
        if (data_in == 0x00)
        {
          m_tx_buffer[4] = 0x01;
          m_tx_buffer[5] = 0x02;
          m_tx_buffer[6] = 0x00;
          m_tx_buffer[7] = 0x0A;
        }
        else if (data_in == 0x01)
        {
          m_tx_buffer[4] = 0x01;
          m_tx_buffer[5] = 0x01;
          m_tx_buffer[6] = 0x01;
          m_tx_buffer[7] = 0x14;
        }
      }
    }
    break;

    case Command::Command47:
    {
      if (m_command_step == 2 && data_in != 0x00)
      {
        m_tx_buffer[4] = 0x00;
        m_tx_buffer[5] = 0x00;
        m_tx_buffer[6] = 0x00;
        m_tx_buffer[7] = 0x00;
      }
    }
    break;

    case Command::Command4C:
    {
      if (m_command_step == 2)
      {
        if (data_in == 0x00)
          m_tx_buffer[5] = 0x04;
        else if (data_in == 0x01)
          m_tx_buffer[5] = 0x02;
      }
    }
    break;

    case Command::GetSetRumble:
    {
      int rumble_index = m_command_step - 2;
      if (rumble_index >= 0)
      {
        m_tx_buffer[m_command_step] = m_rumble_config[rumble_index];
        m_rumble_config[rumble_index] = data_in;

        if (data_in == 0x00)
          m_rumble_config_small_motor_index = rumble_index;
        else if (data_in == 0x01)
          m_rumble_config_large_motor_index = rumble_index;
      }

      if (m_command_step == 7)
      {
        if (m_rumble_config_large_motor_index == -1)
          SetMotorState(LargeMotor, 0);

        if (m_rumble_config_small_motor_index == -1)
          SetMotorState(SmallMotor, 0);
      }
    }
    break;

      DefaultCaseIsUnreachable();
  }

  *data_out = m_tx_buffer[m_command_step];

  m_command_step = (m_command_step + 1) % m_response_length;
  ack = (m_command_step == 0) ? false : true;

  if (m_command_step == 0)
  {
    m_command = Command::Idle;

    DEBUG_LOG("Rx: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", m_rx_buffer[0], m_rx_buffer[1],
              m_rx_buffer[2], m_rx_buffer[3], m_rx_buffer[4], m_rx_buffer[5], m_rx_buffer[6], m_rx_buffer[7]);
    DEBUG_LOG("Tx: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", m_tx_buffer[0], m_tx_buffer[1],
              m_tx_buffer[2], m_tx_buffer[3], m_tx_buffer[4], m_tx_buffer[5], m_tx_buffer[6], m_tx_buffer[7]);

    m_rx_buffer.fill(0x00);
    m_tx_buffer.fill(0x00);
  }

  return ack;
}

std::unique_ptr<NeGconRumble> NeGconRumble::Create(u32 index)
{
  return std::make_unique<NeGconRumble>(index);
}

constinit const Controller::ControllerBindingInfo NeGconRumble::s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   HALFAXIS_BIND_START_INDEX + static_cast<u32>(halfaxis),                                                             \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}
#define MOTOR(name, display_name, icon_name, index, genb)                                                              \
  {name, display_name, icon_name, MOTOR_BIND_START_INDEX + index, InputBindingInfo::Type::Motor, genb}
#define MODE_LED(name, display_name, icon_name, index, genb)                                                           \
  {name, display_name, icon_name, LED_BIND_START_INDEX + index, InputBindingInfo::Type::LED, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGcon", "D-Pad Up"), ICON_PF_DPAD_UP, NeGconRumble::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGcon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, NeGconRumble::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGcon", "D-Pad Down"), ICON_PF_DPAD_DOWN, NeGconRumble::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGcon", "D-Pad Left"), ICON_PF_DPAD_LEFT, NeGconRumble::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGcon", "Start"),ICON_PF_START, NeGconRumble::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGcon", "A Button"), ICON_PF_BUTTON_A, NeGconRumble::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGcon", "B Button"), ICON_PF_BUTTON_B, NeGconRumble::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGcon", "I Button"), ICON_PF_BUTTON_ALT_1, NeGconRumble::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGcon", "II Button"), ICON_PF_BUTTON_ALT_2, NeGconRumble::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGcon", "Left Trigger"), ICON_PF_LEFT_TRIGGER_LT, NeGconRumble::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGcon", "Right Trigger"), ICON_PF_RIGHT_TRIGGER_RT, NeGconRumble::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Left"), ICON_PF_ANALOG_LEFT, NeGconRumble::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Right"), ICON_PF_ANALOG_RIGHT, NeGconRumble::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
  BUTTON("Analog", TRANSLATE_NOOP("NeGcon", "Analog Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, NeGconRumble::Button::Analog, GenericInputBinding::System),
  
  MOTOR("LargeMotor", TRANSLATE_NOOP("NeGcon", "Large Motor"), ICON_PF_VIBRATION_L, LargeMotor, GenericInputBinding::LargeMotor),
  MOTOR("SmallMotor", TRANSLATE_NOOP("NeGcon", "Small Motor"), ICON_PF_VIBRATION, SmallMotor, GenericInputBinding::SmallMotor),

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
  NegConBase::LoadSettings(si, section, initial);
  m_vibration_bias[0] = static_cast<s16>(
    std::clamp(si.GetIntValue(section, "LargeMotorVibrationBias", DEFAULT_LARGE_MOTOR_VIBRATION_BIAS), -255, 255));
  m_vibration_bias[1] = static_cast<s16>(
    std::clamp(si.GetIntValue(section, "SmallMotorVibrationBias", DEFAULT_SMALL_MOTOR_VIBRATION_BIAS), -255, 255));
}
