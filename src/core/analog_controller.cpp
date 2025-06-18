// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "analog_controller.h"
#include "host.h"
#include "settings.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

#include "IconsFontAwesome6.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <cmath>

LOG_CHANNEL(Controller);

AnalogController::AnalogController(u32 index) : Controller(index)
{
  m_status_byte = 0x5A;
  m_axis_state.fill(0x80);
  m_rumble_config.fill(0xFF);
}

AnalogController::~AnalogController() = default;

ControllerType AnalogController::GetType() const
{
  return ControllerType::AnalogController;
}

bool AnalogController::InAnalogMode() const
{
  return m_analog_mode;
}

void AnalogController::Reset()
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
    if (CanStartInAnalogMode(ControllerType::AnalogController))
      SetAnalogMode(true, false);
  }
}

bool AnalogController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  const bool old_analog_mode = m_analog_mode;
  MotorState motor_state = m_motor_state;

  if (sw.GetVersion() < 76) [[unlikely]]
  {
    u8 unused_command_param = 0;
    bool unused_legacy_rumble_unlocked = false;

    sw.Do(&m_analog_mode);
    sw.Do(&m_dualshock_enabled);
    sw.DoEx(&unused_legacy_rumble_unlocked, 44, false);
    sw.Do(&m_configuration_mode);
    sw.Do(&unused_command_param);
    sw.DoEx(&m_status_byte, 55, static_cast<u8>(0x5A));

    u16 button_state = m_button_state;
    sw.DoEx(&button_state, 44, static_cast<u16>(0xFFFF));
    if (apply_input_state)
      m_button_state = button_state;

    sw.Do(&m_command);

    int unused_rumble_config_large_motor_index = -1;
    int unused_rumble_config_small_motor_index = -1;

    sw.DoEx(&m_rumble_config, 45, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    sw.DoEx(&unused_rumble_config_large_motor_index, 45, -1);
    sw.DoEx(&unused_rumble_config_small_motor_index, 45, -1);
    sw.DoEx(&m_analog_toggle_queued, 45, false);

    sw.Do(&motor_state);
  }
  else
  {
    sw.Do(&m_command);
    sw.Do(&m_command_step);
    sw.Do(&m_response_length);
    sw.DoBytes(m_rx_buffer.data(), m_rx_buffer.size());
    sw.DoBytes(m_tx_buffer.data(), m_tx_buffer.size());
    sw.Do(&m_analog_mode);
    sw.Do(&m_analog_locked);
    sw.Do(&m_dualshock_enabled);
    sw.Do(&m_configuration_mode);
    sw.DoBytes(m_rumble_config.data(), m_rumble_config.size());
    sw.Do(&m_status_byte);
    // sw.Do(&m_digital_mode_extra_halfwords); // always zero

    auto axis_state = m_axis_state;
    u16 button_state = m_button_state;
    sw.DoBytes(axis_state.data(), axis_state.size());
    sw.Do(&button_state);
    sw.Do(&motor_state);

    if (apply_input_state)
    {
      m_axis_state = axis_state;
      m_button_state = button_state;
    }
  }

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

float AnalogController::GetBindState(u32 index) const
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

float AnalogController::GetVibrationMotorState(u32 index) const
{
  return ((index < m_motor_state.size()) ? m_motor_state[index] : 0) * (1.0f / 255.0f);
}

void AnalogController::SetBindState(u32 index, float value)
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

u32 AnalogController::GetButtonStateBits() const
{
  // flip bits, native data is active low
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> AnalogController::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::LeftY)] << 24 | m_axis_state[static_cast<size_t>(Axis::LeftX)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::RightY)] << 8 | m_axis_state[static_cast<size_t>(Axis::RightX)];
}

u32 AnalogController::GetInputOverlayIconColor() const
{
  return m_analog_mode ? 0xFF2534F0u : 0xFFCCCCCCu;
}

void AnalogController::ResetTransferState()
{
  if (m_analog_toggle_queued)
  {
    ProcessAnalogModeToggle();
    m_analog_toggle_queued = false;
  }

  m_command = Command::Idle;
  m_command_step = 0;
}

void AnalogController::SetAnalogMode(bool enabled, bool show_message)
{
  if (m_analog_mode == enabled)
    return;

  INFO_LOG("Controller {} switched to {} mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  if (show_message)
  {
    Host::AddIconOSDMessage(
      fmt::format("Controller{}AnalogMode", m_index), ICON_PF_GAMEPAD_ALT,
      enabled ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
                fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
  }

  m_analog_mode = enabled;
}

void AnalogController::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    Host::AddIconOSDMessage(
      fmt::format("Controller{}AnalogMode", m_index), ICON_PF_GAMEPAD_ALT,
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

void AnalogController::SetMotorState(u32 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  if (m_motor_state[motor] != value)
  {
    m_motor_state[motor] = value;
    UpdateHostVibration();
  }
}

void AnalogController::UpdateHostVibration()
{
  std::array<float, NUM_MOTORS> hvalues;
  for (u32 motor = 0; motor < NUM_MOTORS; motor++)
  {
    // Small motor is only 0/1.
    const u8 state =
      (motor == SmallMotor) ? (((m_motor_state[SmallMotor] & 0x01) != 0x00) ? 255 : 0) : m_motor_state[LargeMotor];

    // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
    const double x = static_cast<double>(std::clamp<s32>(static_cast<s32>(state) + m_vibration_bias[motor], 0, 255));
    const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                            156.82454281087692 * x + 3.637978807091713e-11;

    hvalues[motor] = (state != 0) ? static_cast<float>(strength / 65535.0) : 0.0f;
  }

  DEV_LOG("Set small motor to {}, large motor to {}", hvalues[SmallMotor], hvalues[LargeMotor]);
  InputManager::SetPadVibrationIntensity(m_index, hvalues[LargeMotor], hvalues[SmallMotor]);
}

u16 AnalogController::GetExtraButtonMask() const
{
  u16 mask = 0xFFFF;

  static constexpr u8 NEG_THRESHOLD = static_cast<u8>(128.0f - (127.0 * 0.5f));
  static constexpr u8 POS_THRESHOLD = static_cast<u8>(128.0f + (127.0 * 0.5f));

  if (m_analog_dpad_in_digital_mode && !m_analog_mode && !m_configuration_mode)
  {
    const bool left = (m_axis_state[static_cast<u8>(Axis::LeftX)] <= NEG_THRESHOLD);
    const bool right = (m_axis_state[static_cast<u8>(Axis::LeftX)] >= POS_THRESHOLD);
    const bool up = (m_axis_state[static_cast<u8>(Axis::LeftY)] <= NEG_THRESHOLD);
    const bool down = (m_axis_state[static_cast<u8>(Axis::LeftY)] >= POS_THRESHOLD);

    mask = ~((static_cast<u16>(left) << static_cast<u8>(Button::Left)) |
             (static_cast<u16>(right) << static_cast<u8>(Button::Right)) |
             (static_cast<u16>(up) << static_cast<u8>(Button::Up)) |
             (static_cast<u16>(down) << static_cast<u8>(Button::Down)));
  }

  if (m_analog_shoulder_buttons == 2 || (m_analog_shoulder_buttons == 1 && !m_analog_mode && !m_configuration_mode))
  {
    const bool left = (m_axis_state[static_cast<u8>(Axis::RightX)] <= NEG_THRESHOLD);
    const bool right = (m_axis_state[static_cast<u8>(Axis::RightX)] >= POS_THRESHOLD);

    mask &= ~((static_cast<u16>(left) << static_cast<u8>(Button::L1)) |
              (static_cast<u16>(right) << static_cast<u8>(Button::R1)));
  }

  return mask;
}

void AnalogController::ResetRumbleConfig()
{
  m_rumble_config.fill(0xFF);
  SetMotorState(SmallMotor, 0);
  SetMotorState(LargeMotor, 0);
}

u8 AnalogController::GetResponseNumHalfwords() const
{
  if (m_configuration_mode || m_analog_mode)
    return 0x3;

  return (0x1 + m_digital_mode_extra_halfwords);
}

u8 AnalogController::GetModeID() const
{
  if (m_configuration_mode)
    return 0xF;

  if (m_analog_mode)
    return 0x7;

  return 0x4;
}

u8 AnalogController::GetIDByte() const
{
  return Truncate8((GetModeID() << 4) | GetResponseNumHalfwords());
}

void AnalogController::Poll()
{
  // m_tx_buffer = {GetIDByte(), m_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  m_tx_buffer[0] = GetIDByte();
  m_tx_buffer[1] = m_status_byte;

  const u16 button_state = m_button_state & GetExtraButtonMask();
  m_tx_buffer[2] = Truncate8(button_state);
  m_tx_buffer[3] = Truncate8(button_state >> 8);

  if (m_analog_mode || m_configuration_mode)
  {
    m_tx_buffer[4] = m_axis_state[static_cast<u8>(Axis::RightX)];
    m_tx_buffer[5] = m_axis_state[static_cast<u8>(Axis::RightY)];
    m_tx_buffer[6] = m_axis_state[static_cast<u8>(Axis::LeftX)];
    m_tx_buffer[7] = m_axis_state[static_cast<u8>(Axis::LeftY)];
  }
  else
  {
    m_tx_buffer[4] = 0;
    m_tx_buffer[5] = 0;
    m_tx_buffer[6] = 0;
    m_tx_buffer[7] = 0;
  }
}

bool AnalogController::Transfer(const u8 data_in, u8* data_out)
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
        Poll();
      }
      else if (data_in == 0x43)
      {
        Assert(m_command_step == 0);
        m_response_length = (GetResponseNumHalfwords() + 1) * 2;
        m_command = Command::ConfigModeSetMode;
        if (!m_configuration_mode)
          Poll();
        else
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
      if (m_dualshock_enabled)
      {
        if (m_command_step >= 2 && m_command_step < 7)
        {
          const u8 motor_to_set = m_rumble_config[m_command_step - 2];
          if (motor_to_set <= LargeMotor)
            SetMotorState(motor_to_set, data_in);
        }
      }
      else if (m_command_step == 3)
      {
        const bool legacy_rumble_on = (m_rx_buffer[2] & 0xC0) == 0x40 && (m_rx_buffer[3] & 0x01) != 0;
        SetMotorState(SmallMotor, legacy_rumble_on ? 255 : 0);
      }
    }
    break;

    case Command::ConfigModeSetMode:
    {
      if (m_command_step == (static_cast<s32>(m_response_length) - 1))
      {
        m_configuration_mode = (m_rx_buffer[2] == 1);

        if (m_configuration_mode)
        {
          m_dualshock_enabled = true;
          m_status_byte = 0x5A;
        }

        DEBUG_LOG("0x{:02x}({}) config mode", m_rx_buffer[2], m_configuration_mode ? "enter" : "leave");
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
          m_tx_buffer[5] = 0x07;
      }
    }
    break;

    case Command::GetSetRumble:
    {
      if (m_command_step >= 2 && m_command_step < 7)
      {
        const u8 index = m_command_step - 2;
        m_tx_buffer[m_command_step] = m_rumble_config[index];
        m_rumble_config[index] = data_in;

        if (data_in == LargeMotor)
          DEBUG_LOG("Large motor mapped to byte index {}", index);
        else if (data_in == SmallMotor)
          DEBUG_LOG("Small motor mapped to byte index {}", index);
      }
      else if (m_command_step == 7)
      {
        // reset motor value if we're no longer mapping it
        bool has_small = false;
        bool has_large = false;
        for (size_t i = 0; i < m_rumble_config.size(); i++)
        {
          has_small |= (m_rumble_config[i] == SmallMotor);
          has_large |= (m_rumble_config[i] == LargeMotor);
        }
        if (!has_small)
          SetMotorState(SmallMotor, 0);
        if (!has_large)
          SetMotorState(LargeMotor, 0);
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

std::unique_ptr<AnalogController> AnalogController::Create(u32 index)
{
  return std::make_unique<AnalogController>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   static_cast<u32>(AnalogController::Button::Count) + static_cast<u32>(halfaxis),                                     \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}
#define MOTOR(name, display_name, icon_name, index, genb)                                                              \
  {name, display_name, icon_name, index, InputBindingInfo::Type::Motor, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("AnalogController", "D-Pad Up"), ICON_PF_DPAD_UP, AnalogController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("AnalogController", "D-Pad Right"), ICON_PF_DPAD_RIGHT, AnalogController::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("AnalogController", "D-Pad Down"), ICON_PF_DPAD_DOWN, AnalogController::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("AnalogController", "D-Pad Left"), ICON_PF_DPAD_LEFT, AnalogController::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("AnalogController", "Triangle"), ICON_PF_BUTTON_TRIANGLE, AnalogController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("AnalogController", "Circle"), ICON_PF_BUTTON_CIRCLE, AnalogController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("AnalogController", "Cross"), ICON_PF_BUTTON_CROSS, AnalogController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("AnalogController", "Square"), ICON_PF_BUTTON_SQUARE, AnalogController::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("AnalogController", "Select"), ICON_PF_SELECT_SHARE, AnalogController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("AnalogController", "Start"),ICON_PF_START, AnalogController::Button::Start, GenericInputBinding::Start),
  BUTTON("Analog", TRANSLATE_NOOP("AnalogController", "Analog Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, AnalogController::Button::Analog, GenericInputBinding::System),
  BUTTON("L1", TRANSLATE_NOOP("AnalogController", "L1"), ICON_PF_LEFT_SHOULDER_L1, AnalogController::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("AnalogController", "R1"), ICON_PF_RIGHT_SHOULDER_R1, AnalogController::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("AnalogController", "L2"), ICON_PF_LEFT_TRIGGER_L2, AnalogController::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("AnalogController", "R2"), ICON_PF_RIGHT_TRIGGER_R2, AnalogController::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", TRANSLATE_NOOP("AnalogController", "L3"), ICON_PF_LEFT_ANALOG_CLICK, AnalogController::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", TRANSLATE_NOOP("AnalogController", "R3"), ICON_PF_RIGHT_ANALOG_CLICK, AnalogController::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", TRANSLATE_NOOP("AnalogController", "Left Stick Left"), ICON_PF_LEFT_ANALOG_LEFT, AnalogController::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", TRANSLATE_NOOP("AnalogController", "Left Stick Right"), ICON_PF_LEFT_ANALOG_RIGHT, AnalogController::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", TRANSLATE_NOOP("AnalogController", "Left Stick Down"), ICON_PF_LEFT_ANALOG_DOWN, AnalogController::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", TRANSLATE_NOOP("AnalogController", "Left Stick Up"), ICON_PF_LEFT_ANALOG_UP, AnalogController::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", TRANSLATE_NOOP("AnalogController", "Right Stick Left"), ICON_PF_RIGHT_ANALOG_LEFT, AnalogController::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", TRANSLATE_NOOP("AnalogController", "Right Stick Right"), ICON_PF_RIGHT_ANALOG_RIGHT, AnalogController::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", TRANSLATE_NOOP("AnalogController", "Right Stick Down"), ICON_PF_RIGHT_ANALOG_DOWN, AnalogController::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", TRANSLATE_NOOP("AnalogController", "Right Stick Up"), ICON_PF_RIGHT_ANALOG_UP, AnalogController::HalfAxis::RUp, GenericInputBinding::RightStickUp),

  MOTOR("LargeMotor", TRANSLATE_NOOP("AnalogController", "Large Motor"), ICON_PF_VIBRATION_L, 0, GenericInputBinding::LargeMotor),
  MOTOR("SmallMotor", TRANSLATE_NOOP("AnalogController", "Small Motor"), ICON_PF_VIBRATION, 1, GenericInputBinding::SmallMotor),
// clang-format on

#undef MOTOR
#undef AXIS
#undef BUTTON
};

static constexpr const char* s_invert_settings[] = {
  TRANSLATE_NOOP("AnalogController", "Not Inverted"), TRANSLATE_NOOP("AnalogController", "Invert Left/Right"),
  TRANSLATE_NOOP("AnalogController", "Invert Up/Down"),
  TRANSLATE_NOOP("AnalogController", "Invert Left/Right + Up/Down"), nullptr};

static constexpr const char* s_shoulder_settings[] = {
  TRANSLATE_NOOP("AnalogController", "Never"), TRANSLATE_NOOP("AnalogController", "Digital Mode Only"),
  TRANSLATE_NOOP("AnalogController", "Analog and Digital Modes"), nullptr};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "ForceAnalogOnReset", TRANSLATE_NOOP("AnalogController", "Force Analog Mode on Reset"),
   TRANSLATE_NOOP("AnalogController", "Forces the controller to analog mode when the console is reset/powered on."),
   "true", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "AnalogDPadInDigitalMode",
   TRANSLATE_NOOP("AnalogController", "Use Left Analog for D-Pad in Digital Mode"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Allows you to use the left analog stick to control the d-pad in digital mode, as well as the buttons."),
   "true", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::IntegerList, "AnalogShoulderButtons",
   TRANSLATE_NOOP("AnalogController", "Use Right Analog for Shoulder Buttons"),
   TRANSLATE_NOOP("AnalogController",
                  "Allows you to use the right analog stick to control the shoulder buttons, as well as the buttons."),
   "0", "0", "2", nullptr, nullptr, s_shoulder_settings, 0.0f},
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("AnalogController", "Analog Deadzone"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0", "0", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("AnalogController", "Analog Sensitivity"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33", "0.01", "2", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ButtonDeadzone", TRANSLATE_NOOP("AnalogController", "Button/Trigger Deadzone"),
   TRANSLATE_NOOP("AnalogController", "Sets the deadzone for activating buttons/triggers, "
                                      "i.e. the fraction of the trigger which will be ignored."),
   "0.25", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "LargeMotorVibrationBias",
   TRANSLATE_NOOP("AnalogController", "Large Motor Vibration Bias"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the bias value for the large vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::Integer, "SmallMotorVibrationBias",
   TRANSLATE_NOOP("AnalogController", "Small Motor Vibration Bias"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the bias value for the small vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::IntegerList, "InvertLeftStick", TRANSLATE_NOOP("AnalogController", "Invert Left Stick"),
   TRANSLATE_NOOP("AnalogController", "Inverts the direction of the left analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "InvertRightStick", TRANSLATE_NOOP("AnalogController", "Invert Right Stick"),
   TRANSLATE_NOOP("AnalogController", "Inverts the direction of the right analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
};

const Controller::ControllerInfo AnalogController::INFO = {ControllerType::AnalogController,
                                                           "AnalogController",
                                                           TRANSLATE_NOOP("ControllerType", "Analog Controller"),
                                                           ICON_PF_GAMEPAD_ALT,
                                                           s_binding_info,
                                                           s_settings};

void AnalogController::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  Controller::LoadSettings(si, section, initial);
  m_force_analog_on_reset = si.GetBoolValue(section, "ForceAnalogOnReset", true);
  m_analog_dpad_in_digital_mode = si.GetBoolValue(section, "AnalogDPadInDigitalMode", true);
  m_analog_shoulder_buttons = static_cast<u8>(si.GetUIntValue(section, "AnalogShoulderButtons", 0u));
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_button_deadzone = std::clamp(si.GetFloatValue(section, "ButtonDeadzone", DEFAULT_BUTTON_DEADZONE), 0.01f, 1.0f);
  m_vibration_bias[0] = static_cast<s16>(
    std::clamp(si.GetIntValue(section, "LargeMotorVibrationBias", DEFAULT_LARGE_MOTOR_VIBRATION_BIAS), -255, 255));
  m_vibration_bias[1] = static_cast<s16>(
    std::clamp(si.GetIntValue(section, "SmallMotorVibrationBias", DEFAULT_SMALL_MOTOR_VIBRATION_BIAS), -255, 255));
  m_invert_left_stick = static_cast<u8>(si.GetUIntValue(section, "InvertLeftStick", 0u));
  m_invert_right_stick = static_cast<u8>(si.GetUIntValue(section, "InvertRightStick", 0u));
}
