#include "analog_controller.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "system.h"
Log_SetChannel(AnalogController);

AnalogController::AnalogController(u32 index) : m_index(index)
{
  m_axis_state.fill(0x80);
}

AnalogController::~AnalogController() = default;

ControllerType AnalogController::GetType() const
{
  return ControllerType::AnalogController;
}

void AnalogController::Reset()
{
  m_analog_mode = false;
  m_rumble_unlocked = false;
  m_configuration_mode = false;
  m_command_param = 0;

  if (m_auto_enable_analog)
    SetAnalogMode(true);
}

bool AnalogController::DoState(StateWrapper& sw)
{
  if (!Controller::DoState(sw))
    return false;

  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);
  sw.Do(&m_rumble_unlocked);
  sw.Do(&m_configuration_mode);
  sw.Do(&m_command_param);
  sw.Do(&m_state);

  MotorState motor_state = m_motor_state;
  sw.Do(&motor_state);

  if (sw.IsReading())
  {
    for (u8 i = 0; i < NUM_MOTORS; i++)
      SetMotorState(i, motor_state[i]);

    if (old_analog_mode != m_analog_mode)
    {
      g_system->GetHostInterface()->AddFormattedOSDMessage(2.0f, "Controller %u switched to %s mode.", m_index + 1u,
                                                           m_analog_mode ? "analog" : "digital");
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

void AnalogController::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // -1..1 -> 0..255
  const u8 u8_value = static_cast<u8>(std::clamp(((value + 1.0f) / 2.0f) * 255.0f, 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void AnalogController::SetAxisState(Axis axis, u8 value)
{
  m_axis_state[static_cast<u8>(axis)] = value;
}

void AnalogController::SetButtonState(Button button, bool pressed)
{
  if (button == Button::Analog)
  {
    // analog toggle
    if (pressed)
    {
      if (m_analog_locked)
      {
        g_system->GetHostInterface()->AddFormattedOSDMessage(2.0f, "Controller %u is locked to %s mode by the game.",
                                                             m_index + 1u, m_analog_mode ? "analog" : "digital");
      }
      else
      {
        SetAnalogMode(!m_analog_mode);
      }
    }

    return;
  }

  if (pressed)
    m_button_state &= ~(u16(1) << static_cast<u8>(button));
  else
    m_button_state |= u16(1) << static_cast<u8>(button);
}

void AnalogController::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

u32 AnalogController::GetVibrationMotorCount() const
{
  return NUM_MOTORS;
}

float AnalogController::GetVibrationMotorStrength(u32 motor)
{
  DebugAssert(motor < NUM_MOTORS);
  return static_cast<float>(m_motor_state[motor]) * (1.0f / 255.0f);
}

void AnalogController::ResetTransferState()
{
  m_state = State::Idle;
}

u16 AnalogController::GetID() const
{
  static constexpr u16 DIGITAL_MODE_ID = 0x5A41;
  static constexpr u16 ANALOG_MODE_ID = 0x5A73;
  static constexpr u16 CONFIG_MODE_ID = 0x5AF3;

  if (m_configuration_mode)
    return CONFIG_MODE_ID;

  return m_analog_mode ? ANALOG_MODE_ID : DIGITAL_MODE_ID;
}

void AnalogController::SetAnalogMode(bool enabled)
{
  if (m_analog_mode == enabled)
    return;

  Log_InfoPrintf("Controller %u switched to %s mode.", m_index + 1u, enabled ? "analog" : "digital");
  g_system->GetHostInterface()->AddFormattedOSDMessage(2.0f, "Controller %u switched to %s mode.", m_index + 1u,
                                                       enabled ? "analog" : "digital");
  m_analog_mode = enabled;
}

void AnalogController::SetMotorState(u8 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  m_motor_state[motor] = value;
}

bool AnalogController::Transfer(const u8 data_in, u8* data_out)
{
  bool ack;
#ifdef _DEBUG
  u8 old_state = static_cast<u8>(m_state);
#endif

  switch (m_state)
  {
#define FIXED_REPLY_STATE(state, reply, ack_value, next_state)                                                         \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = reply;                                                                                                 \
    m_state = next_state;                                                                                              \
    ack = ack_value;                                                                                                   \
  }                                                                                                                    \
  break;

#define ID_STATE_MSB(state, next_state)                                                                                \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = Truncate8(GetID() >> 8);                                                                               \
    m_state = next_state;                                                                                              \
    ack = true;                                                                                                        \
  }                                                                                                                    \
  break;

    case State::Idle:
    {
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(GetID());
        m_state = State::GetStateIDMSB;
        ack = true;
      }
      else if (data_in == 0x43)
      {
        *data_out = Truncate8(GetID());
        m_state = State::ConfigModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x44)
      {
        *data_out = Truncate8(GetID());
        m_state = State::SetAnalogModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x45)
      {
        *data_out = Truncate8(GetID());
        m_state = State::GetAnalogModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x46)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command46IDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x47)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command47IDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x4C)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command4CIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x4D)
      {
        m_rumble_unlocked = true;
        *data_out = Truncate8(GetID());
        m_state = State::UnlockRumbleIDMSB;
        ack = true;
      }
      else
      {
        Log_DebugPrintf("data_in = 0x%02X", data_in);
        *data_out = 0xFF;
        ack = (data_in == 0x01);
      }
    }
    break;

      ID_STATE_MSB(State::GetStateIDMSB, State::GetStateButtonsLSB);

    case State::GetStateButtonsLSB:
    {
      if (m_rumble_unlocked)
        SetMotorState(1, ((data_in & 0x01) != 0) ? 255 : 0);

      *data_out = Truncate8(m_button_state);
      m_state = State::GetStateButtonsMSB;
      ack = true;
    }
    break;

    case State::GetStateButtonsMSB:
    {
      if (m_rumble_unlocked)
        SetMotorState(0, (data_in != 0) ? 255 : 0);

      *data_out = Truncate8(m_button_state >> 8);
      m_state = m_analog_mode ? State::GetStateRightAxisX : State::Idle;
      ack = m_analog_mode;
    }
    break;

      FIXED_REPLY_STATE(State::GetStateRightAxisX, Truncate8(m_axis_state[static_cast<u8>(Axis::RightX)]), true,
                        State::GetStateRightAxisY);
      FIXED_REPLY_STATE(State::GetStateRightAxisY, Truncate8(m_axis_state[static_cast<u8>(Axis::RightY)]), true,
                        State::GetStateLeftAxisX);
      FIXED_REPLY_STATE(State::GetStateLeftAxisX, Truncate8(m_axis_state[static_cast<u8>(Axis::LeftX)]), true,
                        State::GetStateLeftAxisY);
      FIXED_REPLY_STATE(State::GetStateLeftAxisY, Truncate8(m_axis_state[static_cast<u8>(Axis::LeftY)]), false,
                        State::Idle);

      ID_STATE_MSB(State::ConfigModeIDMSB, State::ConfigModeSetMode);

    case State::ConfigModeSetMode:
    {
      Log_DebugPrintf("0x%02x(%s) config mode", data_in, data_in == 1 ? "enter" : "leave");
      m_configuration_mode = (data_in == 1);
      *data_out = Truncate8(m_button_state);
      m_state = State::GetStateButtonsMSB;
      ack = true;
    }
    break;

      ID_STATE_MSB(State::SetAnalogModeIDMSB, State::SetAnalogModeVal);

    case State::SetAnalogModeVal:
    {
      Log_DevPrintf("analog mode val 0x%02x", data_in);
      if (data_in == 0x00 || data_in == 0x01)
        SetAnalogMode((data_in == 0x01));

      *data_out = 0x00;
      m_state = State::SetAnalogModeSel;
      ack = true;
    }
    break;

    case State::SetAnalogModeSel:
    {
      Log_DevPrintf("analog mode lock 0x%02x", data_in);
      if (data_in == 0x02 || data_in == 0x03)
        m_analog_locked = (data_in == 0x03);

      *data_out = 0x00;
      m_state = State::Pad4Bytes;
      ack = true;
    }
    break;

      ID_STATE_MSB(State::GetAnalogModeIDMSB, State::GetAnalogMode1);
      FIXED_REPLY_STATE(State::GetAnalogMode1, 0x01, true, State::GetAnalogMode2);
      FIXED_REPLY_STATE(State::GetAnalogMode2, 0x02, true, State::GetAnalogMode3);
      FIXED_REPLY_STATE(State::GetAnalogMode3, BoolToUInt8(m_analog_mode), true, State::GetAnalogMode4);
      FIXED_REPLY_STATE(State::GetAnalogMode4, 0x02, true, State::GetAnalogMode5);
      FIXED_REPLY_STATE(State::GetAnalogMode5, 0x01, true, State::GetAnalogMode6);
      FIXED_REPLY_STATE(State::GetAnalogMode6, 0x00, false, State::Idle);

      ID_STATE_MSB(State::Command46IDMSB, State::Command461);

    case State::Command461:
    {
      Log_DebugPrintf("command 46 param 0x%02X", data_in);
      m_command_param = data_in;
      *data_out = 0x00;
      m_state = State::Command462;
      ack = true;
    }
    break;

      FIXED_REPLY_STATE(State::Command462, 0x00, true, State::Command463);
      FIXED_REPLY_STATE(State::Command463, 0x01, true, State::Command464);
      FIXED_REPLY_STATE(State::Command464, ((m_command_param == 1) ? 1 : 2), true, State::Command465);
      FIXED_REPLY_STATE(State::Command465, ((m_command_param == 1) ? 1 : 0), true, State::Command466);
      FIXED_REPLY_STATE(State::Command466, ((m_command_param == 1) ? 0x14 : 0x0A), false, State::Idle);

      ID_STATE_MSB(State::Command47IDMSB, State::Command471);
      FIXED_REPLY_STATE(State::Command471, 0x00, true, State::Command472);
      FIXED_REPLY_STATE(State::Command472, 0x00, true, State::Command473);
      FIXED_REPLY_STATE(State::Command473, 0x02, true, State::Command474);
      FIXED_REPLY_STATE(State::Command474, 0x00, true, State::Command475);
      FIXED_REPLY_STATE(State::Command475, 0x01, true, State::Command476);
      FIXED_REPLY_STATE(State::Command476, 0x00, false, State::Idle);

      ID_STATE_MSB(State::Command4CIDMSB, State::Command4CMode);

    case State::Command4CMode:
    {
      // SetAnalogMode(data_in != 0x00);
      // Log_WarningPrintf("analog mode %s by 0x4c", m_analog_mode ? "enabled" : "disabled");
      *data_out = 0x00;
      m_state = State::Command4C1;
      ack = true;
    }
    break;

      FIXED_REPLY_STATE(State::Command4C1, 0x00, true, State::Command4C2);
      FIXED_REPLY_STATE(State::Command4C2, 0x00, true, State::Command4C3);
      FIXED_REPLY_STATE(State::Command4C3, m_analog_mode ? 0x07 : 0x04, true, State::Command4C4);
      FIXED_REPLY_STATE(State::Command4C4, 0x00, true, State::Command4C5);
      FIXED_REPLY_STATE(State::Command4C5, 0x00, false, State::Idle);

      ID_STATE_MSB(State::UnlockRumbleIDMSB, State::Pad6Bytes);

      FIXED_REPLY_STATE(State::Pad6Bytes, 0x00, true, State::Pad5Bytes);
      FIXED_REPLY_STATE(State::Pad5Bytes, 0x00, true, State::Pad4Bytes);
      FIXED_REPLY_STATE(State::Pad4Bytes, 0x00, true, State::Pad3Bytes);
      FIXED_REPLY_STATE(State::Pad3Bytes, 0x00, true, State::Pad2Bytes);
      FIXED_REPLY_STATE(State::Pad2Bytes, 0x00, true, State::Pad1Byte);
      FIXED_REPLY_STATE(State::Pad1Byte, 0x00, false, State::Idle);

    default:
    {
      UnreachableCode();
      return false;
    }
  }

  Log_DebugPrintf("Transfer, old_state=%u, new_state=%u, data_in=0x%02X, data_out=0x%02X, ack=%s",
                  static_cast<u32>(old_state), static_cast<u32>(m_state), data_in, *data_out, ack ? "true" : "false");
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
#define A(n)                                                                                                           \
  {                                                                                                                    \
#n, static_cast < s32>(Axis::n)                                                                                    \
  }

  return {A(LeftX), A(LeftY), A(RightX), A(RightY)};

#undef A
}

Controller::ButtonList AnalogController::StaticGetButtonNames()
{
#define B(n)                                                                                                           \
  {                                                                                                                    \
#n, static_cast < s32>(Button::n)                                                                                  \
  }
  return {B(Up),     B(Down), B(Left), B(Right), B(Select), B(Start), B(Triangle), B(Cross), B(Circle),
          B(Square), B(L1),   B(L2),   B(R1),    B(R2),     B(L3),    B(R3),       B(Analog)};
#undef B
}

u32 AnalogController::StaticGetVibrationMotorCount()
{
  return NUM_MOTORS;
}

Controller::SettingList AnalogController::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 2> settings = {
    {{SettingInfo::Type::Boolean, "AutoEnableAnalog", "Enable Analog Mode on Reset",
      "Automatically enables analog mode when the console is reset/powered on.", "false"},
     {SettingInfo::Type::Float, "AxisScale", "Analog Axis Scale",
      "Sets the analog stick axis scaling factor. A value between 1.30 and 1.40 is recommended when using recent "
      "controllers, e.g. DualShock 4, Xbox One Controller.",
      "1.00f", "0.01f", "1.50f", "0.01f"}}};

  return SettingList(settings.begin(), settings.end());
}

void AnalogController::LoadSettings(HostInterface* host_interface, const char* section)
{
  Controller::LoadSettings(host_interface, section);
  m_auto_enable_analog = host_interface->GetBoolSettingValue(section, "AutoEnableAnalog", false);
}
