// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "config_mode_controller.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <algorithm>
#include <cmath>

LOG_CHANNEL(Controller);

ConfigModeController::ConfigModeController(u32 index, ControllerType type) : Controller(index, type)
{
  m_rumble_config.fill(0xFF);
}

ConfigModeController::~ConfigModeController() = default;

bool ConfigModeController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  bool analog_mode = m_analog_mode;
  MotorState motor_state = m_motor_state;
  sw.Do(&m_command, Command::UnusedConfig);
  sw.Do(&analog_mode);
  sw.Do(&m_analog_locked);
  sw.Do(&m_dualshock_enabled);
  sw.Do(&m_configuration_mode);
  sw.Do(&m_analog_toggle_queued);
  sw.Do(&m_status_byte);
  sw.Do(&m_rumble_config);
  sw.Do(&motor_state);

  if (sw.IsReading())
    FinishLoadState(analog_mode, motor_state);

  return !sw.HasError();
}

void ConfigModeController::FinishLoadState(bool analog_mode, const MotorState& motor_state)
{
  SetAnalogMode(analog_mode, true);

  for (u32 i = 0; i < GetNumMotors(); i++)
  {
    if (m_motor_state[i] != motor_state[i])
      SetMotorState(i, motor_state[i]);
  }
}

void ConfigModeController::ResetTransferState()
{
  if (m_analog_toggle_queued)
  {
    ProcessAnalogModeToggle();
    m_analog_toggle_queued = false;
  }

  Controller::ResetTransferState();
}

void ConfigModeController::ResetConfigState()
{
  Controller::ResetTransferState();
  m_analog_mode = false;
  m_analog_locked = false;
  m_configuration_mode = false;
  m_dualshock_enabled = false;
  ResetRumbleConfig();
  m_status_byte = 0x5A;
}

u8 ConfigModeController::GetAnalogModeID() const
{
  return 0x7;
}

u8 ConfigModeController::GetNumMotors() const
{
  return MAX_MOTORS;
}

u8 ConfigModeController::GetCommand45Actuators() const
{
  return 0x02;
}

void ConfigModeController::PatchCommand4C(u8 param)
{
  if (param == 0x00)
    m_tx_buffer[5] = 0x04;
  else if (param == 0x01)
    m_tx_buffer[5] = 0x07;
}

bool ConfigModeController::HasLegacyRumble() const
{
  return true;
}

bool ConfigModeController::CanEnterConfigMode() const
{
  return true;
}

void ConfigModeController::SetMotorState(u32 motor, u8 value)
{
  DebugAssert(motor < MAX_MOTORS);
  if (m_motor_state[motor] != value)
  {
    m_motor_state[motor] = value;

    const float hvalue = GetMotorStrength(motor);
    DEV_LOG("Set {} motor to {} (raw {})", (motor == LargeMotor) ? "large" : "small", hvalue, m_motor_state[motor]);
    InputManager::SetPadVibrationIntensity(m_index, GetMotorBindIndex(motor), hvalue);
  }
}

float ConfigModeController::GetMotorStrength(u32 motor) const
{
  // Small motor is only 0/1.
  const u8 state =
    (motor == SmallMotor) ? (((m_motor_state[SmallMotor] & 0x01) != 0x00) ? 255 : 0) : m_motor_state[LargeMotor];

  // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
  const double x = static_cast<double>(std::clamp<s32>(static_cast<s32>(state) + m_vibration_bias[motor], 0, 255));
  const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                          156.82454281087692 * x + 3.637978807091713e-11;

  return (state != 0) ? static_cast<float>(strength / 65535.0) : 0.0f;
}

void ConfigModeController::ResetRumbleConfig()
{
  m_rumble_config.fill(0xFF);
  for (u32 i = 0; i < GetNumMotors(); i++)
    SetMotorState(i, 0);
}

void ConfigModeController::SetAnalogMode(bool enabled, bool show_message)
{
  if (m_analog_mode == enabled)
    return;

  m_analog_mode = enabled;

  InputManager::SetPadLEDState(m_index, BoolToFloat(enabled));

  INFO_LOG("Controller {} switched to {} mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  if (show_message)
    ShowModeMessage(enabled);
}

void ConfigModeController::ShowModeMessage(bool enabled)
{
  Host::AddIconOSDMessage(
    OSDMessageType::Quick, fmt::format("Controller{}AnalogMode", m_index), ICON_PF_GAMEPAD_ALT,
    enabled ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
              fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
}

void ConfigModeController::ToggleAnalogMode()
{
  if (m_transfer_state == TransferState::Idle)
    ProcessAnalogModeToggle();
  else
    m_analog_toggle_queued = true;
}

void ConfigModeController::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Quick, fmt::format("Controller{}AnalogMode", m_index), ICON_PF_GAMEPAD_ALT,
      fmt::format(m_analog_mode ? TRANSLATE_FS("Controller", "Controller {} is locked to analog mode by the game.") :
                                  TRANSLATE_FS("Controller", "Controller {} is locked to digital mode by the game."),
                  m_index + 1u));
  }
  else
  {
    SetAnalogMode(!m_analog_mode, true);
    ResetRumbleConfig();

    // Set status byte to 0 if we were previously in configuration mode, so that the game knows about the mode change.
    if (m_dualshock_enabled)
    {
      // However, the problem with doing this unconditionally is that games like Tomb Raider have the loader menu
      // put the pad into configuration mode, but not analog mode. So if the user toggles analog mode, the status
      // byte ends up stuck at 0x00. As a workaround, clear out config/dualshock mode when the game isn't flagged
      // as supporting the dualshock.
      if (!m_analog_mode && !CanStartInAnalogMode(ControllerType::AnalogController))
      {
        WARNING_LOG("Resetting pad on digital->analog switch.");
        m_configuration_mode = false;
        m_dualshock_enabled = false;
        m_status_byte = 0x5A;
      }
      else
      {
        m_status_byte = 0x00;
      }
    }
  }
}

u8 ConfigModeController::GetResponseNumHalfwords() const
{
  if (m_configuration_mode || m_analog_mode)
    return 0x3;

  // Actuators mapped past the first two motor bytes extend the reply in digital mode, to include the halfword
  // with that byte. The motor bytes start at the third byte after the command.
  for (u32 i = static_cast<u32>(m_rumble_config.size()) - 1; i >= 2; i--)
  {
    if (m_rumble_config[i] < GetNumMotors())
      return static_cast<u8>((i + 2) / 2);
  }

  return 0x1;
}

u8 ConfigModeController::GetIDByte() const
{
  const u8 mode_id = m_configuration_mode ? 0xF : (m_analog_mode ? GetAnalogModeID() : 0x4);
  return Truncate8((mode_id << 4) | GetResponseNumHalfwords());
}

bool ConfigModeController::BeginCommand(u8 command)
{
  m_tx_buffer[1] = m_status_byte;

  if (command == 0x42)
  {
    m_command = Command::ReadPad;
    Poll();
    return true;
  }
  else if (command == 0x43 && (m_configuration_mode || CanEnterConfigMode()))
  {
    m_command = Command::ConfigModeSetMode;
    if (!m_configuration_mode)
      Poll();

    return true;
  }
  else if (!m_configuration_mode)
  {
    return false;
  }

  switch (command)
  {
    case 0x44:
      m_command = Command::SetAnalogMode;
      ResetRumbleConfig();
      return true;

    case 0x45:
      m_command = Command::GetAnalogMode;
      m_tx_buffer[2] = 0x01;
      m_tx_buffer[3] = 0x02;
      m_tx_buffer[4] = BoolToUInt8(m_analog_mode);
      m_tx_buffer[5] = GetCommand45Actuators();
      m_tx_buffer[6] = 0x01;
      return true;

    case 0x46:
      m_command = Command::Command46;
      return true;

    case 0x47:
      m_command = Command::Command47;
      m_tx_buffer[4] = 0x02;
      m_tx_buffer[6] = 0x01;
      return true;

    case 0x48:
      m_command = Command::Command48;
      return true;

    case 0x4C:
      m_command = Command::Command4C;
      return true;

    case 0x4D:
      m_command = Command::GetSetRumble;
      return true;

    case 0x40:
    case 0x41:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4E:
    case 0x4F:
      // DualShock 2 and unused commands, these only return zeros on PSX pads.
      m_command = Command::UnusedConfig;
      return true;

    default:
      ERROR_LOG("Unimplemented config mode command 0x{:02X}", command);
      return false;
  }
}

void ConfigModeController::ProcessCommandByte(u32 step, u8 data_in)
{
  switch (m_command)
  {
    case Command::ReadPad:
    {
      if (m_dualshock_enabled)
      {
        if (step >= 2)
        {
          const u8 motor_to_set = m_rumble_config[step - 2];
          if (motor_to_set < GetNumMotors())
            SetMotorState(motor_to_set, data_in);
        }
      }
      else if (step == 3 && HasLegacyRumble())
      {
        const bool legacy_rumble_on = (m_rx_buffer[2] & 0xC0) == 0x40 && (m_rx_buffer[3] & 0x01) != 0;
        SetMotorState(SmallMotor, legacy_rumble_on ? 255 : 0);
      }
    }
    break;

    case Command::SetAnalogMode:
    {
      if (step == 2)
      {
        DEV_LOG("analog mode val 0x{:02x}", data_in);

        if (data_in == 0x00 || data_in == 0x01)
          SetAnalogMode((data_in == 0x01), true);
      }
      else if (step == 3)
      {
        DEV_LOG("analog mode lock 0x{:02x}", data_in);

        // 00h-02h unlock, 03h locks, and higher values are masked.
        m_analog_locked = ((data_in & 0x03) == 0x03);
      }
    }
    break;

    case Command::Command46:
    {
      if (step == 2)
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
      if (step == 2 && data_in != 0x00)
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
      if (step == 2)
        PatchCommand4C(data_in);
    }
    break;

    case Command::Command48:
    {
      if (step == 2 && data_in <= 0x01)
        m_tx_buffer[6] = 0x01;
    }
    break;

    case Command::GetSetRumble:
    {
      if (step >= 2)
      {
        const u32 index = step - 2;
        m_tx_buffer[step] = m_rumble_config[index];
        m_rumble_config[index] = data_in;

        if (data_in < GetNumMotors())
          DEBUG_LOG("Motor {} mapped to byte index {}", data_in, index);
      }
    }
    break;

    default:
      break;
  }
}

void ConfigModeController::EndCommand()
{
  if (m_command == Command::ConfigModeSetMode)
  {
    m_configuration_mode = (m_rx_buffer[2] == 1);

    if (m_configuration_mode)
    {
      m_dualshock_enabled = true;
      m_status_byte = 0x5A;
    }

    DEBUG_LOG("0x{:02x}({}) config mode", m_rx_buffer[2], m_configuration_mode ? "enter" : "leave");
  }
  else if (m_command == Command::GetSetRumble)
  {
    // reset motor value if we're no longer mapping it
    for (u32 i = 0; i < GetNumMotors(); i++)
    {
      if (std::ranges::find(m_rumble_config, Truncate8(i)) == m_rumble_config.end())
        SetMotorState(i, 0);
    }
  }
}
