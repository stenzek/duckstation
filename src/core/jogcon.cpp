// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "IconsFontAwesome5.h"
#include "jogcon.h"
#include "common/log.h"
#include "common/string_util.h"
#include "host.h"
#include "settings.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <cmath>
Log_SetChannel(JogCon);

JogCon::JogCon(u32 index) : Controller(index)
{
  m_status_byte = 0x5A;
  m_axis_state.fill(0x80);
  m_rumble_config.fill(0xFF);
}

JogCon::~JogCon() = default;

ControllerType JogCon::GetType() const
{
  return ControllerType::JogCon;
}

bool JogCon::InAnalogMode() const
{
  return m_analog_mode;
}

void JogCon::Reset()
{
  m_command = Command::Idle;
  m_command_step = 0;
  m_rx_buffer.fill(0x00);
  m_tx_buffer.fill(0x00);
  m_analog_mode = false;
  m_configuration_mode = false;

  jog_position = 0x0;
  jog_direction = JogconDirection::JOGCON_DIR_NONE;
  jog_rotations = 0x0;
  jog_last_command = JogconCommand::JOGCON_CMD_NONE;

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
    if (g_settings.controller_disable_analog_mode_forcing || System::IsRunningBIOS())
    {
      Host::AddIconOSDMessage(
        fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
        Host::TranslateStdString(
          "OSDMessage", "Analog mode forcing is disabled by game settings. Controller will start in digital mode."),
        10.0f);
    }
    else
    {
      SetAnalogMode(true, false);
    }
  }
}

bool JogCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);
  sw.Do(&m_dualshock_enabled);
  sw.DoEx(&m_legacy_rumble_unlocked, 44, false);
  sw.Do(&m_configuration_mode);
  sw.Do(&m_command_param);
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
      Host::AddIconOSDMessage(
        fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
        fmt::format((m_analog_mode ?
                       Host::TranslateString("JogCon", "Controller {} switched to analog mode.") :
                       Host::TranslateString("JogCon", "Controller {} switched to digital mode."))
                      .GetCharArray(),
                    m_index + 1u),
        5.0f);
    }
  }
  return true;
}

float JogCon::GetBindState(u32 index) const
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

void JogCon::SetBindState(u32 index, float value)
{
  if (index == static_cast<s32>(Button::Analog))
  {
    // analog toggle
    if (value >= m_button_deadzone)
    {
      if (m_command == Command::Idle)
        ProcessAnalogModeToggle();
      else
        m_analog_toggle_queued = true;
    }

    return;
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    const u32 sub_index = index - static_cast<u32>(Button::Count);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return;

    const u8 u8_value = static_cast<u8>(std::clamp(value * m_analog_sensitivity * 255.0f, 0.0f, 255.0f));
    if (u8_value == m_half_axis_state[sub_index])
      return;

    m_half_axis_state[sub_index] = u8_value;
    System::SetRunaheadReplayFlag();

#define MERGE(pos, neg)                                                                                                \
  ((m_half_axis_state[static_cast<u32>(pos)] != 0) ? (127u + ((m_half_axis_state[static_cast<u32>(pos)] + 1u) / 2u)) : \
                                                     (127u - (m_half_axis_state[static_cast<u32>(neg)] / 2u)))
    switch (static_cast<HalfAxis>(sub_index))
    {
      case HalfAxis::LLeft:
      case HalfAxis::LRight:
        m_axis_state[static_cast<u8>(Axis::LeftX)] = ((m_invert_left_stick & 1u) != 0u) ?
                                                       MERGE(HalfAxis::LLeft, HalfAxis::LRight) :
                                                       MERGE(HalfAxis::LRight, HalfAxis::LLeft);
        break;

      case HalfAxis::LDown:
      case HalfAxis::LUp:
        m_axis_state[static_cast<u8>(Axis::LeftY)] = ((m_invert_left_stick & 2u) != 0u) ?
                                                       MERGE(HalfAxis::LUp, HalfAxis::LDown) :
                                                       MERGE(HalfAxis::LDown, HalfAxis::LUp);
        break;

      case HalfAxis::RLeft:
      case HalfAxis::RRight:
        m_axis_state[static_cast<u8>(Axis::RightX)] = ((m_invert_right_stick & 1u) != 0u) ?
                                                        MERGE(HalfAxis::RLeft, HalfAxis::RRight) :
                                                        MERGE(HalfAxis::RRight, HalfAxis::RLeft);
        break;

      case HalfAxis::RDown:
      case HalfAxis::RUp:
        m_axis_state[static_cast<u8>(Axis::RightY)] = ((m_invert_right_stick & 2u) != 0u) ?
                                                        MERGE(HalfAxis::RUp, HalfAxis::RDown) :
                                                        MERGE(HalfAxis::RDown, HalfAxis::RUp);
        break;

      default:
        break;
    }

    if (m_analog_deadzone > 0.0f)
    {
#define MERGE_F(pos, neg)                                                                                              \
  ((m_half_axis_state[static_cast<u32>(pos)] != 0) ?                                                                   \
     (static_cast<float>(m_half_axis_state[static_cast<u32>(pos)]) / 255.0f) :                                         \
     (static_cast<float>(m_half_axis_state[static_cast<u32>(neg)]) / -255.0f))

      float pos_x, pos_y;
      if (static_cast<HalfAxis>(sub_index) < HalfAxis::RLeft)
      {
        pos_x = ((m_invert_left_stick & 1u) != 0u) ? MERGE_F(HalfAxis::LLeft, HalfAxis::LRight) :
                                                     MERGE_F(HalfAxis::LRight, HalfAxis::LLeft);
        pos_y = ((m_invert_left_stick & 2u) != 0u) ? MERGE_F(HalfAxis::LUp, HalfAxis::LDown) :
                                                     MERGE_F(HalfAxis::LDown, HalfAxis::LUp);
      }
      else
      {
        pos_x = ((m_invert_right_stick & 1u) != 0u) ? MERGE_F(HalfAxis::RLeft, HalfAxis::RRight) :
                                                      MERGE_F(HalfAxis::RRight, HalfAxis::RLeft);
        ;
        pos_y = ((m_invert_right_stick & 2u) != 0u) ? MERGE_F(HalfAxis::RUp, HalfAxis::RDown) :
                                                      MERGE_F(HalfAxis::RDown, HalfAxis::RUp);
      }

      if (InCircularDeadzone(m_analog_deadzone, pos_x, pos_y))
      {
        // Set to 127 (center).
        if (static_cast<HalfAxis>(sub_index) < HalfAxis::RLeft)
          m_axis_state[static_cast<u8>(Axis::LeftX)] = m_axis_state[static_cast<u8>(Axis::LeftY)] = 127;
        else
          m_axis_state[static_cast<u8>(Axis::RightX)] = m_axis_state[static_cast<u8>(Axis::RightY)] = 127;
      }
#undef MERGE_F
    }

#undef MERGE

    return;
  }

  const u16 bit = u16(1) << static_cast<u8>(index);

  if (value >= m_button_deadzone)
  {
    if (m_button_state & bit)
      System::SetRunaheadReplayFlag();

    m_button_state &= ~(bit);
  }
  else
  {
    if (!(m_button_state & bit))
      System::SetRunaheadReplayFlag();

    m_button_state |= bit;
  }
}

u32 JogCon::GetButtonStateBits() const
{
  // flip bits, native data is active low
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> JogCon::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::LeftY)] << 24 | m_axis_state[static_cast<size_t>(Axis::LeftX)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::RightY)] << 8 | m_axis_state[static_cast<size_t>(Axis::RightX)];
}

void JogCon::ResetTransferState()
{
  if (m_analog_toggle_queued)
  {
    ProcessAnalogModeToggle();
    m_analog_toggle_queued = false;
  }

  m_command = Command::Idle;
  m_command_step = 0;
}

void JogCon::SetAnalogMode(bool enabled, bool show_message)
{
  if (m_analog_mode == enabled)
    return;

  Log_InfoPrintf("Controller %u switched to %s mode.", m_index + 1u, enabled ? "analog" : "digital");
  if (show_message)
  {
    Host::AddIconOSDMessage(
      fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
      fmt::format((enabled ? Host::TranslateString("JogCon", "Controller {} switched to analog mode.") :
                             Host::TranslateString("JogCon", "Controller {} switched to digital mode."))
                    .GetCharArray(),
                  m_index + 1u),
      5.0f);
  }
  m_analog_mode = enabled;
}

void JogCon::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    Host::AddIconOSDMessage(
      fmt::format("Controller{}AnalogMode", m_index), ICON_FA_GAMEPAD,
      fmt::format((m_analog_mode ?
                     Host::TranslateString("JogCon", "Controller {} is locked to analog mode by the game.") :
                     Host::TranslateString("JogCon", "Controller {} is locked to digital mode by the game."))
                    .GetCharArray(),
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

void JogCon::SetMotorState(u32 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  if (m_motor_state[motor] != value)
  {
    m_motor_state[motor] = value;
    // UpdateHostVibration();

    JogconCommand command = static_cast<JogconCommand>((value & 0xC0) >> 4);
    JogconDirection direction = static_cast<JogconDirection>((value & 0x30) >> 4);
    u8 force = (value & 0x0F);

    jog_last_command = command;

    Log_InfoPrintf("Jogcon %u motor 0x%x: command %s, direction %s, force %x.", m_index + 1u, value,
                   command == JogconCommand::JOGCON_CMD_NONE ? "NONE" : (command == JogconCommand::JOGCON_CMD_DROP_REVOLUTIONS ? "DROP_REV" : (command == JogconCommand::JOGCON_CMD_NEW_START ? "NEW_START" : "OTHER")),
                   direction == JogconDirection::JOGCON_DIR_NONE ? "NONE" :
                     (direction == JogconDirection::JOGCON_DIR_CW ? "CW" : (direction == JogconDirection::JOGCON_DIR_CCW ? "CCW" : (direction == JogconDirection::JOGCON_DIR_START ? "START" : "OTHER"))),
                   force);
  }
}

void JogCon::UpdateHostVibration()
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

  Host::SetPadVibrationIntensity(m_index, hvalues[0], hvalues[1]);
}

u8 JogCon::GetExtraButtonMaskLSB() const
{
  return 0xFF;
  /*if (!m_analog_dpad_in_digital_mode || m_analog_mode || m_configuration_mode)
    return 0xFF;

  static constexpr u8 NEG_THRESHOLD = static_cast<u8>(128.0f - (127.0 * 0.5f));
  static constexpr u8 POS_THRESHOLD = static_cast<u8>(128.0f + (127.0 * 0.5f));

  const bool left = (m_axis_state[static_cast<u8>(Axis::LeftX)] <= NEG_THRESHOLD);
  const bool right = (m_axis_state[static_cast<u8>(Axis::LeftX)] >= POS_THRESHOLD);
  const bool up = (m_axis_state[static_cast<u8>(Axis::LeftY)] <= NEG_THRESHOLD);
  const bool down = (m_axis_state[static_cast<u8>(Axis::LeftY)] >= POS_THRESHOLD);

  return ~((static_cast<u8>(left) << static_cast<u8>(Button::Left)) |
           (static_cast<u8>(right) << static_cast<u8>(Button::Right)) |
           (static_cast<u8>(up) << static_cast<u8>(Button::Up)) |
           (static_cast<u8>(down) << static_cast<u8>(Button::Down)));*/
}

void JogCon::ResetRumbleConfig()
{
  m_rumble_config.fill(0xFF);

  m_rumble_config_large_motor_index = -1;
  m_rumble_config_small_motor_index = -1;

  SetMotorState(LargeMotor, 0);
  SetMotorState(SmallMotor, 0);
}

void JogCon::SetMotorStateForConfigIndex(int index, u8 value)
{
  if (m_rumble_config_small_motor_index == index)
    // SetMotorState(SmallMotor, ((value & 0x01) != 0) ? 255 : 0);
    SetMotorState(SmallMotor, value);
  else if (m_rumble_config_large_motor_index == index)
    SetMotorState(LargeMotor, value);
}

u8 JogCon::GetResponseNumHalfwords() const
{
  if (m_configuration_mode || m_analog_mode)
    return 0x3;

  return (0x1 + m_digital_mode_extra_halfwords);
}

u8 JogCon::GetModeID() const
{
  if (m_configuration_mode)
    return 0xF;

  if (m_analog_mode)
    return 0xE;

  return 0x4;
}

u8 JogCon::GetIDByte() const
{
  return Truncate8((GetModeID() << 4) | GetResponseNumHalfwords());
}

bool JogCon::Transfer(const u8 data_in, u8* data_out)
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
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x01, 0x02, BoolToUInt8(m_analog_mode), 0x01, 0x01, 0x00};
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
        m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
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

        case 4: // 5 position
        {
          if (m_axis_state[static_cast<u8>(Axis::LeftX)] > jog_position)
          {
            jog_direction = JogconDirection::JOGCON_DIR_CW;
          }
          else if (m_axis_state[static_cast<u8>(Axis::LeftX)] < jog_position)
          {
            jog_direction = JogconDirection::JOGCON_DIR_CCW;
          }
          else
          {
            jog_direction = JogconDirection::JOGCON_DIR_NONE;
          }

          if (m_axis_state[static_cast<u8>(Axis::LeftX)] - 0x80 < 0x00)
            jog_rotations = 0xFF;

          jog_position = m_axis_state[static_cast<u8>(Axis::LeftX)];

          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftX)] - 0x80;

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 5: // 6 number of rotations
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = jog_rotations; // m_axis_state[static_cast<u8>(Axis::RightY)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 6: // 7 last command and rotation direction
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = (static_cast<u8>(jog_last_command) << 4) | static_cast<u8>(jog_direction);

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);

          // clear jogcon stuff
          jog_rotations = 0x0;
          jog_direction = JogconDirection::JOGCON_DIR_NONE;
          jog_last_command = JogconCommand::JOGCON_CMD_NONE;
        }
        break;

        case 7: // 8
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = 0x00; // m_axis_state[static_cast<u8>(Axis::LeftY)];

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
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftX)] - 0x80;
          }
          break;

          case 5:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = 0x00; // m_axis_state[static_cast<u8>(Axis::RightY)];
          }
          break;

          case 6:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = static_cast<u8>(jog_direction);
          }
          break;

          case 7:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = 0x00; // m_axis_state[static_cast<u8>(Axis::LeftY)];
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
          m_tx_buffer[4] = 0x04;
          m_tx_buffer[5] = 0x03;
          m_tx_buffer[6] = 0x01;
          m_tx_buffer[7] = 0x1E;
        }
        else if (data_in == 0x01)
        {
          m_tx_buffer[4] = 0x00;
          m_tx_buffer[5] = 0x00;
          m_tx_buffer[6] = 0x00;
          m_tx_buffer[7] = 0x00;
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
          m_tx_buffer[4] = 0x03;
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

std::unique_ptr<JogCon> JogCon::Create(u32 index)
{
  return std::make_unique<JogCon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, button, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb                                 \
  }
#define AXIS(name, display_name, halfaxis, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(JogCon::Button::Count) + static_cast<u32>(halfaxis),                \
      InputBindingInfo::Type::HalfAxis, genb                                                                           \
  }

  BUTTON("Up", "D-Pad Up", JogCon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", "D-Pad Right", JogCon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", "D-Pad Down", JogCon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", "D-Pad Left", JogCon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", "Triangle", JogCon::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", "Circle", JogCon::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", "Cross", JogCon::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", "Square", JogCon::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", "Select", JogCon::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", "Start", JogCon::Button::Start, GenericInputBinding::Start),
  BUTTON("Analog", "Analog Toggle", JogCon::Button::Analog, GenericInputBinding::System),
  BUTTON("L1", "L1", JogCon::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", "R1", JogCon::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", "L2", JogCon::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", "R2", JogCon::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", "L3", JogCon::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", "R3", JogCon::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", "Left Stick Left", JogCon::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", "Left Stick Right", JogCon::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", "Left Stick Down", JogCon::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", "Left Stick Up", JogCon::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", "Right Stick Left", JogCon::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", "Right Stick Right", JogCon::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", "Right Stick Down", JogCon::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", "Right Stick Up", JogCon::HalfAxis::RUp, GenericInputBinding::RightStickUp),

#undef AXIS
#undef BUTTON
};

static const char* s_invert_settings[] = {TRANSLATABLE("JogCon", "Not Inverted"),
                                          TRANSLATABLE("JogCon", "Invert Left/Right"),
                                          TRANSLATABLE("JogCon", "Invert Up/Down"),
                                          TRANSLATABLE("JogCon", "Invert Left/Right + Up/Down"), nullptr};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "ForceAnalogOnReset", TRANSLATABLE("JogCon", "Force Analog Mode on Reset"),
   TRANSLATABLE("JogCon", "Forces the controller to analog mode when the console is reset/powered on. May "
                                    "cause issues with games, so it is recommended to leave this option off."),
   "true"},
  {SettingInfo::Type::Boolean, "AnalogDPadInDigitalMode",
   TRANSLATABLE("JogCon", "Use Analog Sticks for D-Pad in Digital Mode"),
   TRANSLATABLE("JogCon",
                "Allows you to use the analog sticks to control the d-pad in digital mode, as well as the buttons."),
   "true"},
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATABLE("JogCon", "Analog Deadzone"),
   TRANSLATABLE("JogCon",
                "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0.00f", "0.00f", "1.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATABLE("JogCon", "Analog Sensitivity"),
   TRANSLATABLE(
     "JogCon",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33f", "0.01f", "2.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ButtonDeadzone", TRANSLATABLE("JogCon", "Button/Trigger Deadzone"),
   TRANSLATABLE("JogCon", "Sets the deadzone for activating buttons/triggers, "
                                    "i.e. the fraction of the trigger which will be ignored."),
   "0.25", "0.00", "1.00", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "VibrationBias", TRANSLATABLE("JogCon", "Vibration Bias"),
   TRANSLATABLE("JogCon", "Sets the rumble bias value. If rumble in some games is too weak or not "
                                    "functioning, try increasing this value."),
   "8", "0", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::IntegerList, "InvertLeftStick", TRANSLATABLE("JogCon", "Invert Left Stick"),
   TRANSLATABLE("JogCon", "Inverts the direction of the left analog stick."), "0", "0", "3", nullptr, nullptr,
   s_invert_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "InvertRightStick", TRANSLATABLE("JogCon", "Invert Right Stick"),
   TRANSLATABLE("JogCon", "Inverts the direction of the right analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
};

const Controller::ControllerInfo JogCon::INFO = {ControllerType::JogCon,
                                                           "JogCon",
                                                           TRANSLATABLE("ControllerType", "JogCon"),
                                                           s_binding_info,
                                                           countof(s_binding_info),
                                                           s_settings,
                                                           countof(s_settings),
                                                           Controller::VibrationCapabilities::LargeSmallMotors};

void JogCon::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_force_analog_on_reset = si.GetBoolValue(section, "ForceAnalogOnReset", true);
  m_analog_dpad_in_digital_mode = si.GetBoolValue(section, "AnalogDPadInDigitalMode", true);
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_button_deadzone = std::clamp(si.GetFloatValue(section, "ButtonDeadzone", DEFAULT_BUTTON_DEADZONE), 0.0f, 1.0f);
  m_rumble_bias = static_cast<u8>(std::min<u32>(si.GetIntValue(section, "VibrationBias", 8), 255));
  m_invert_left_stick = static_cast<u8>(si.GetIntValue(section, "InvertLeftStick", 0));
  m_invert_right_stick = static_cast<u8>(si.GetIntValue(section, "InvertRightStick", 0));
}
