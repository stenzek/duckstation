#include "analog_controller.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "settings.h"
#include "system.h"
#include <cmath>
Log_SetChannel(AnalogController);

AnalogController::AnalogController(u32 index) : m_index(index)
{
  m_axis_state.fill(0x80);
  Reset();
}

AnalogController::~AnalogController() = default;

ControllerType AnalogController::GetType() const
{
  return ControllerType::AnalogController;
}

void AnalogController::Reset()
{
  m_command = Command::Idle;
  m_command_step = 0;
  m_rx_buffer.fill(0x00);
  m_tx_buffer.fill(0x00);
  m_analog_mode = false;
  m_configuration_mode = false;
  m_motor_state.fill(0);

  m_dualshock_enabled = false;
  ResetRumbleConfig();

  m_status_byte = 0x5A;

  if (m_force_analog_on_reset)
  {
    if (g_settings.controller_disable_analog_mode_forcing)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString(
          "OSDMessage", "Analog mode forcing is disabled by game settings. Controller will start in digital mode."),
        10.0f);
    }
    else
      SetAnalogMode(true);
  }
}

bool AnalogController::DoState(StateWrapper& sw, bool apply_input_state)
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
      g_host_interface->AddFormattedOSDMessage(
        5.0f,
        m_analog_mode ?
          g_host_interface->TranslateString("AnalogController", "Controller %u switched to analog mode.") :
          g_host_interface->TranslateString("AnalogController", "Controller %u switched to digital mode."),
        m_index + 1u);
    }
  }
  return true;
}

std::optional<s32> AnalogController::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> AnalogController::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

float AnalogController::GetAxisState(s32 axis_code) const
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return 0.0f;

  // 0..255 -> -1..1
  const float value = (((static_cast<float>(m_axis_state[static_cast<s32>(axis_code)]) / 255.0f) * 2.0f) - 1.0f);
  return std::clamp(value / m_axis_scale, -1.0f, 1.0f);
}

void AnalogController::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // -1..1 -> 0..255
  const float scaled_value = std::clamp(value * m_axis_scale, -1.0f, 1.0f);
  const u8 u8_value = static_cast<u8>(std::clamp(std::round(((scaled_value + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void AnalogController::SetAxisState(Axis axis, u8 value)
{
  if (value != m_axis_state[static_cast<u8>(axis)])
    System::SetRunaheadReplayFlag();

  m_axis_state[static_cast<u8>(axis)] = value;
}

bool AnalogController::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Analog))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void AnalogController::SetButtonState(Button button, bool pressed)
{
  if (button == Button::Analog)
  {
    // analog toggle
    if (pressed)
    {
      if (m_command == Command::Idle)
        ProcessAnalogModeToggle();
      else
        m_analog_toggle_queued = true;
    }

    return;
  }

  const u16 bit = u16(1) << static_cast<u8>(button);

  if (pressed)
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

void AnalogController::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
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

u32 AnalogController::GetVibrationMotorCount() const
{
  return NUM_MOTORS;
}

float AnalogController::GetVibrationMotorStrength(u32 motor)
{
  DebugAssert(motor < NUM_MOTORS);
  if (m_motor_state[motor] == 0)
    return 0.0f;

  // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
  const double x =
    static_cast<double>(std::min<u32>(static_cast<u32>(m_motor_state[motor]) + static_cast<u32>(m_rumble_bias), 255));
  const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                          156.82454281087692 * x + 3.637978807091713e-11;

  return static_cast<float>(strength / 65535.0);
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

void AnalogController::SetAnalogMode(bool enabled)
{
  if (m_analog_mode == enabled)
    return;

  Log_InfoPrintf("Controller %u switched to %s mode.", m_index + 1u, enabled ? "analog" : "digital");
  g_host_interface->AddFormattedOSDMessage(
    5.0f,
    enabled ? g_host_interface->TranslateString("AnalogController", "Controller %u switched to analog mode.") :
              g_host_interface->TranslateString("AnalogController", "Controller %u switched to digital mode."),
    m_index + 1u);
  m_analog_mode = enabled;
}

void AnalogController::ProcessAnalogModeToggle()
{
  if (m_analog_locked)
  {
    g_host_interface->AddFormattedOSDMessage(
      5.0f,
      m_analog_mode ?
        g_host_interface->TranslateString("AnalogController", "Controller %u is locked to analog mode by the game.") :
        g_host_interface->TranslateString("AnalogController", "Controller %u is locked to digital mode by the game."),
      m_index + 1u);
  }
  else
  {
    SetAnalogMode(!m_analog_mode);
    ResetRumbleConfig();

    if (m_dualshock_enabled)
      m_status_byte = 0x00;
  }
}

void AnalogController::SetMotorState(u8 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  m_motor_state[motor] = value;
}

u8 AnalogController::GetExtraButtonMaskLSB() const
{
  if (!m_analog_dpad_in_digital_mode || m_analog_mode || m_configuration_mode)
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
           (static_cast<u8>(down) << static_cast<u8>(Button::Down)));
}

void AnalogController::ResetRumbleConfig()
{
  m_rumble_config.fill(0xFF);

  m_rumble_config_large_motor_index = -1;
  m_rumble_config_small_motor_index = -1;

  SetMotorState(LargeMotor, 0);
  SetMotorState(SmallMotor, 0);
}

void AnalogController::SetMotorStateForConfigIndex(int index, u8 value)
{
  if (m_rumble_config_small_motor_index == index)
    SetMotorState(SmallMotor, ((value & 0x01) != 0) ? 255 : 0);
  else if (m_rumble_config_large_motor_index == index)
    SetMotorState(LargeMotor, value);
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
        Log_DevPrintf("ACK controller access");
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
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::RightX)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 5:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::RightY)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 6:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftX)];

          if (m_dualshock_enabled)
            SetMotorStateForConfigIndex(rumble_index, data_in);
        }
        break;

        case 7:
        {
          if (m_configuration_mode || m_analog_mode)
            m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftY)];

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
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::RightX)];
          }
          break;

          case 5:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::RightY)];
          }
          break;

          case 6:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftX)];
          }
          break;

          case 7:
          {
            if (m_configuration_mode || m_analog_mode)
              m_tx_buffer[m_command_step] = m_axis_state[static_cast<u8>(Axis::LeftY)];
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
          SetAnalogMode((data_in == 0x01));
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
          m_tx_buffer[5] = 0x07;
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

    Log_DevPrintf("Rx: %02x %02x %02x %02x %02x %02x %02x %02x", m_rx_buffer[0], m_rx_buffer[1], m_rx_buffer[2],
                  m_rx_buffer[3], m_rx_buffer[4], m_rx_buffer[5], m_rx_buffer[6], m_rx_buffer[7]);
    Log_DevPrintf("Tx: %02x %02x %02x %02x %02x %02x %02x %02x", m_tx_buffer[0], m_tx_buffer[1], m_tx_buffer[2],
                  m_tx_buffer[3], m_tx_buffer[4], m_tx_buffer[5], m_tx_buffer[6], m_tx_buffer[7]);

    m_rx_buffer.fill(0x00);
    m_tx_buffer.fill(0x00);
  }

  return ack;
}

std::unique_ptr<AnalogController> AnalogController::Create(u32 index)
{
  return std::make_unique<AnalogController>(index);
}

std::optional<s32> AnalogController::StaticGetAxisCodeByName(std::string_view axis_name)
{
#define AXIS(name)                                                                                                     \
  if (axis_name == #name)                                                                                              \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Axis::name)));                                                \
  }

  AXIS(LeftX);
  AXIS(LeftY);
  AXIS(RightX);
  AXIS(RightY);

  return std::nullopt;

#undef AXIS
}

std::optional<s32> AnalogController::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Select);
  BUTTON(L3);
  BUTTON(R3);
  BUTTON(Start);
  BUTTON(Up);
  BUTTON(Right);
  BUTTON(Down);
  BUTTON(Left);
  BUTTON(L2);
  BUTTON(R2);
  BUTTON(L1);
  BUTTON(R1);
  BUTTON(Triangle);
  BUTTON(Circle);
  BUTTON(Cross);
  BUTTON(Square);
  BUTTON(Analog);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList AnalogController::StaticGetAxisNames()
{
  return {{TRANSLATABLE("AnalogController", "LeftX"), static_cast<s32>(Axis::LeftX), AxisType::Full},
          {TRANSLATABLE("AnalogController", "LeftY"), static_cast<s32>(Axis::LeftY), AxisType::Full},
          {TRANSLATABLE("AnalogController", "RightX"), static_cast<s32>(Axis::RightX), AxisType::Full},
          {TRANSLATABLE("AnalogController", "RightY"), static_cast<s32>(Axis::RightY), AxisType::Full}};
}

Controller::ButtonList AnalogController::StaticGetButtonNames()
{
  return {{TRANSLATABLE("AnalogController", "Up"), static_cast<s32>(Button::Up)},
          {TRANSLATABLE("AnalogController", "Down"), static_cast<s32>(Button::Down)},
          {TRANSLATABLE("AnalogController", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("AnalogController", "Right"), static_cast<s32>(Button::Right)},
          {TRANSLATABLE("AnalogController", "Select"), static_cast<s32>(Button::Select)},
          {TRANSLATABLE("AnalogController", "Start"), static_cast<s32>(Button::Start)},
          {TRANSLATABLE("AnalogController", "Triangle"), static_cast<s32>(Button::Triangle)},
          {TRANSLATABLE("AnalogController", "Cross"), static_cast<s32>(Button::Cross)},
          {TRANSLATABLE("AnalogController", "Circle"), static_cast<s32>(Button::Circle)},
          {TRANSLATABLE("AnalogController", "Square"), static_cast<s32>(Button::Square)},
          {TRANSLATABLE("AnalogController", "L1"), static_cast<s32>(Button::L1)},
          {TRANSLATABLE("AnalogController", "L2"), static_cast<s32>(Button::L2)},
          {TRANSLATABLE("AnalogController", "R1"), static_cast<s32>(Button::R1)},
          {TRANSLATABLE("AnalogController", "R2"), static_cast<s32>(Button::R2)},
          {TRANSLATABLE("AnalogController", "L3"), static_cast<s32>(Button::L3)},
          {TRANSLATABLE("AnalogController", "R3"), static_cast<s32>(Button::R3)},
          {TRANSLATABLE("AnalogController", "Analog"), static_cast<s32>(Button::Analog)}};
}

u32 AnalogController::StaticGetVibrationMotorCount()
{
  return NUM_MOTORS;
}

Controller::SettingList AnalogController::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 4> settings = {
    {{SettingInfo::Type::Boolean, "ForceAnalogOnReset", TRANSLATABLE("AnalogController", "Force Analog Mode on Reset"),
      TRANSLATABLE("AnalogController", "Forces the controller to analog mode when the console is reset/powered on. May "
                                       "cause issues with games, so it is recommended to leave this option off."),
      "false"},
     {SettingInfo::Type::Boolean, "AnalogDPadInDigitalMode",
      TRANSLATABLE("AnalogController", "Use Analog Sticks for D-Pad in Digital Mode"),
      TRANSLATABLE("AnalogController",
                   "Allows you to use the analog sticks to control the d-pad in digital mode, as well as the buttons."),
      "false"},
     {SettingInfo::Type::Float, "AxisScale", TRANSLATABLE("AnalogController", "Analog Axis Scale"),
      TRANSLATABLE(
        "AnalogController",
        "Sets the analog stick axis scaling factor. A value between 1.30 and 1.40 is recommended when using recent "
        "controllers, e.g. DualShock 4, Xbox One Controller."),
      "1.00f", "0.01f", "1.50f", "0.01f"},
     {SettingInfo::Type::Integer, "VibrationBias", TRANSLATABLE("AnalogController", "Vibration Bias"),
      TRANSLATABLE("AnalogController", "Sets the rumble bias value. If rumble in some games is too weak or not "
                                       "functioning, try increasing this value."),
      "8", "0", "255", "1"}}};

  return SettingList(settings.begin(), settings.end());
}

void AnalogController::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_force_analog_on_reset = g_host_interface->GetBoolSettingValue(section, "ForceAnalogOnReset", false);
  m_analog_dpad_in_digital_mode = g_host_interface->GetBoolSettingValue(section, "AnalogDPadInDigitalMode", false);
  m_axis_scale =
    std::clamp(std::abs(g_host_interface->GetFloatSettingValue(section, "AxisScale", 1.00f)), 0.01f, 1.50f);
  m_rumble_bias =
    static_cast<u8>(std::min<u32>(g_host_interface->GetIntSettingValue(section, "VibrationBias", 8), 255));
}
