// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "negcon_rumble.h"
#include "IconsFontAwesome5.h"
#include "common/assert.h"
#include "common/log.h"
#include "host.h"
#include "settings.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

#include "IconsFontAwesome5.h"
#include "IconsPromptFont.h"

#include <cmath>

Log_SetChannel(NeGconRumble);

// Mapping of Button to index of corresponding bit in m_button_state
static constexpr std::array<u8, static_cast<size_t>(NeGconRumble::Button::Count)> s_button_indices = {3, 4,  5,  6,
                                                                                                      7, 11, 12, 13};
NeGconRumble::NeGconRumble(u32 index) : Controller(index)
{
  m_status_byte = 0x5A;
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
  m_rumble_config.fill(0xFF);
}

NeGconRumble::~NeGconRumble() = default;

ControllerType NeGconRumble::GetType() const
{
  return ControllerType::NeGconRumble;
}
bool NeGconRumble::InAnalogMode() const
{
  return m_analog_mode;
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

  if (m_force_analog_on_reset)
  {
    if (g_settings.controller_disable_analog_mode_forcing || System::IsRunningUnknownGame())
    {
      Host::AddIconOSDMessage(
        fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
        TRANSLATE_STR("OSDMessage",
                      "Analog mode forcing is disabled by game settings. Controller will start in digital mode."),
        10.0f);
    }
    else
    {
      SetAnalogMode(true, false);
    }
  }
}

bool NeGconRumble::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

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
      Host::AddIconOSDMessage(fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
                              fmt::format(m_analog_mode ?
                                            TRANSLATE_FS("AnalogController", "Controller {} switched to analog mode.") :
                                            TRANSLATE_FS("AnalogController", "Controller {} switched to digital mode."),
                                          m_index + 1u),
                              5.0f);
    }
  }
  return true;
}

float NeGconRumble::GetBindState(u32 index) const
{
  if (index >= static_cast<u32>(Button::Count))
  {
    const u32 sub_index = index - static_cast<u32>(Button::Count);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return 0.0f;

    return static_cast<float>(m_half_axis_state[sub_index]) * (1.0f / 255.0f);
  }
  else if (index < static_cast<u32>(Button::Analog))
  {
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  }
  else
  {
    return 0.0f;
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
  // Steering Axis: -1..1 -> 0..255
  else if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)) ||
           index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    value *= m_steering_sensitivity;
    if (value < m_steering_deadzone)
      value = 0.0f;

    m_half_axis_state[index - static_cast<u32>(Button::Count)] =
      static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));

    // Merge left/right. Seems to be inverted.
    m_axis_state[static_cast<u32>(Axis::Steering)] =
      ((m_half_axis_state[1] != 0) ? (127u + ((m_half_axis_state[1] + 1u) / 2u)) :
                                     (127u - (m_half_axis_state[0] / 2u)));
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (static_cast<u32>(Button::Count) + 1);
    if (sub_index >= m_axis_state.size())
      return;

    m_axis_state[sub_index] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));
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

u32 NeGconRumble::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> NeGconRumble::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::L)] << 24 | m_axis_state[static_cast<size_t>(Axis::II)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::I)] << 8 | m_axis_state[static_cast<size_t>(Axis::Steering)];
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

  Log_InfoPrintf("Controller %u switched to %s mode.", m_index + 1u, enabled ? "analog" : "digital");
  if (show_message)
  {
    Host::AddIconOSDMessage(fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
                            fmt::format(enabled ?
                                          TRANSLATE_FS("AnalogController", "Controller {} switched to analog mode.") :
                                          TRANSLATE_FS("AnalogController", "Controller {} switched to digital mode."),
                                        m_index + 1u),
                            5.0f);
  }
  m_analog_mode = enabled;
}

void NeGconRumble::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    Host::AddIconOSDMessage(
      fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
      fmt::format(m_analog_mode ?
                    TRANSLATE_FS("AnalogController", "Controller {} is locked to analog mode by the game.") :
                    TRANSLATE_FS("AnalogController", "Controller {} is locked to digital mode by the game."),
                  m_index + 1u),
      5.0f);
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
    UpdateHostVibration();
  }
}

void NeGconRumble::UpdateHostVibration()
{
  std::array<float, NUM_MOTORS> hvalues;
  for (u32 motor = 0; motor < NUM_MOTORS; motor++)
  {
    // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
    const u8 state = m_motor_state[motor];
    const double x = static_cast<double>(std::min<u32>(state + static_cast<u32>(m_rumble_bias), 255));
    const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                            156.82454281087692 * x + 3.637978807091713e-11;

    hvalues[motor] = (state != 0) ? static_cast<float>(strength / 65535.0) : 0.0f;
  }

  InputManager::SetPadVibrationIntensity(m_index, hvalues[0], hvalues[1]);
}

u8 NeGconRumble::GetExtraButtonMaskLSB() const
{
  return 0xFF;
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
        Log_DebugPrintf("ACK controller access");
        m_command = Command::Ready;
        return true;
      }

      Log_DevPrintf("Unknown data_in = 0x%02X", data_in);
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
          Log_ErrorPrintf("Unimplemented config mode command 0x%02X", data_in);

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
          m_tx_buffer[m_command_step] = Truncate8(m_button_state) & GetExtraButtonMaskLSB();

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
            m_tx_buffer[m_command_step] = Truncate8(m_button_state) & GetExtraButtonMaskLSB();
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

        Log_DevPrintf("0x%02x(%s) config mode", m_rx_buffer[2], m_configuration_mode ? "enter" : "leave");
      }
    }
    break;

    case Command::SetAnalogMode:
    {
      if (m_command_step == 2)
      {
        Log_DevPrintf("analog mode val 0x%02x", data_in);

        if (data_in == 0x00 || data_in == 0x01)
          SetAnalogMode((data_in == 0x01), true);
      }
      else if (m_command_step == 3)
      {
        Log_DevPrintf("analog mode lock 0x%02x", data_in);

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

    Log_DebugPrintf("Rx: %02x %02x %02x %02x %02x %02x %02x %02x", m_rx_buffer[0], m_rx_buffer[1], m_rx_buffer[2],
                    m_rx_buffer[3], m_rx_buffer[4], m_rx_buffer[5], m_rx_buffer[6], m_rx_buffer[7]);
    Log_DebugPrintf("Tx: %02x %02x %02x %02x %02x %02x %02x %02x", m_tx_buffer[0], m_tx_buffer[1], m_tx_buffer[2],
                    m_tx_buffer[3], m_tx_buffer[4], m_tx_buffer[5], m_tx_buffer[6], m_tx_buffer[7]);

    m_rx_buffer.fill(0x00);
    m_tx_buffer.fill(0x00);
  }

  return ack;
}

std::unique_ptr<NeGconRumble> NeGconRumble::Create(u32 index)
{
  return std::make_unique<NeGconRumble>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb                      \
  }
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(NeGconRumble::Button::Count) + static_cast<u32>(halfaxis),         \
      InputBindingInfo::Type::HalfAxis, genb                                                                           \
  }

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGconRumble", "D-Pad Up"), ICON_PF_DPAD_UP, NeGconRumble::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGconRumble", "D-Pad Right"), ICON_PF_DPAD_RIGHT, NeGconRumble::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGconRumble", "D-Pad Down"), ICON_PF_DPAD_DOWN, NeGconRumble::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGconRumble", "D-Pad Left"), ICON_PF_DPAD_LEFT, NeGconRumble::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGconRumble", "Start"),ICON_PF_START, NeGconRumble::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGconRumble", "A Button"), ICON_PF_BUTTON_A, NeGconRumble::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGconRumble", "B Button"), ICON_PF_BUTTON_B, NeGconRumble::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGconRumble", "I Button"), ICON_PF_RIGHT_TRIGGER_R2, NeGconRumble::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGconRumble", "II Button"), ICON_PF_LEFT_TRIGGER_L2, NeGconRumble::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGconRumble", "Left Trigger"), ICON_PF_LEFT_ANALOG_LEFT, NeGconRumble::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGconRumble", "Right Trigger"), ICON_PF_RIGHT_SHOULDER_R1, NeGconRumble::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGconRumble", "Steering (Twist) Left"), ICON_PF_LEFT_ANALOG_LEFT, NeGconRumble::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGconRumble", "Steering (Twist) Right"), ICON_PF_LEFT_ANALOG_LEFT, NeGconRumble::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
  BUTTON("Analog", TRANSLATE_NOOP("NeGconRumble", "Analog Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, NeGconRumble::Button::Analog, GenericInputBinding::System),
// clang-format on

#undef AXIS
#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATE_NOOP("NeGconRumble", "Steering Axis Deadzone"),
   TRANSLATE_NOOP("NeGconRumble", "Sets deadzone size for steering axis."), "0.00f", "0.00f", "0.99f", "0.01f",
   "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringSensitivity", TRANSLATE_NOOP("NeGconRumble", "Steering Axis Sensitivity"),
   TRANSLATE_NOOP("NeGconRumble", "Sets the steering axis scaling factor."), "1.00f", "0.01f", "2.00f", "0.01f",
   "%.0f%%", nullptr, 100.0f},
};

const Controller::ControllerInfo NeGconRumble::INFO = {ControllerType::NeGconRumble,
                                                       "NeGconRumble",
                                                       TRANSLATE_NOOP("ControllerType", "NeGcon with Rumble"),
                                                       ICON_PF_GAMEPAD,
                                                       s_binding_info,
                                                       s_settings,
                                                       Controller::VibrationCapabilities::LargeSmallMotors};

void NeGconRumble::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_steering_deadzone = si.GetFloatValue(section, "SteeringDeadzone", 0.10f);
  m_steering_sensitivity = si.GetFloatValue(section, "SteeringSensitivity", 1.00f);
  m_rumble_bias = static_cast<u8>(std::min<u32>(si.GetIntValue(section, "VibrationBias", 8), 255));
}