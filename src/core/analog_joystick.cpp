#include "analog_joystick.h"
#include "common/log.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <cmath>
Log_SetChannel(AnalogJoystick);

AnalogJoystick::AnalogJoystick(u32 index)
{
  m_index = index;
  m_axis_state.fill(0x80);
  Reset();
}

AnalogJoystick::~AnalogJoystick() = default;

ControllerType AnalogJoystick::GetType() const
{
  return ControllerType::AnalogJoystick;
}

void AnalogJoystick::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool AnalogJoystick::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);

  u16 button_state = m_button_state;
  auto axis_state = m_axis_state;
  sw.Do(&button_state);
  sw.Do(&axis_state);

  if (apply_input_state)
  {
    m_button_state = button_state;
    m_axis_state = axis_state;
  }

  sw.Do(&m_transfer_state);

  if (sw.IsReading() && (old_analog_mode != m_analog_mode))
  {
    g_host_interface->AddFormattedOSDMessage(
      5.0f,
      m_analog_mode ? g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                      g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
      m_index + 1u);
  }
  return true;
}

std::optional<s32> AnalogJoystick::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> AnalogJoystick::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

float AnalogJoystick::GetAxisState(s32 axis_code) const
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return 0.0f;

  // 0..255 -> -1..1
  const float value = (((static_cast<float>(m_axis_state[static_cast<s32>(axis_code)]) / 255.0f) * 2.0f) - 1.0f);
  return std::clamp(value / m_axis_scale, -1.0f, 1.0f);
}

void AnalogJoystick::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // -1..1 -> 0..255
  const float scaled_value = std::clamp(value * m_axis_scale, -1.0f, 1.0f);
  const u8 u8_value = static_cast<u8>(std::clamp(std::round(((scaled_value + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void AnalogJoystick::SetAxisState(Axis axis, u8 value)
{
  if (m_axis_state[static_cast<u8>(axis)] != value)
    System::SetRunaheadReplayFlag();

  m_axis_state[static_cast<u8>(axis)] = value;
}

bool AnalogJoystick::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void AnalogJoystick::SetButtonState(Button button, bool pressed)
{
  if (button == Button::Mode)
  {
    if (pressed)
      ToggleAnalogMode();

    return;
  }

  const u16 bit = u16(1) << static_cast<u8>(button);

  if (pressed)
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

void AnalogJoystick::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

u32 AnalogJoystick::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> AnalogJoystick::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::LeftY)] << 24 | m_axis_state[static_cast<size_t>(Axis::LeftX)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::RightY)] << 8 | m_axis_state[static_cast<size_t>(Axis::RightX)];
}

void AnalogJoystick::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

u16 AnalogJoystick::GetID() const
{
  static constexpr u16 DIGITAL_MODE_ID = 0x5A41;
  static constexpr u16 ANALOG_MODE_ID = 0x5A53;

  return m_analog_mode ? ANALOG_MODE_ID : DIGITAL_MODE_ID;
}

void AnalogJoystick::ToggleAnalogMode()
{
  m_analog_mode = !m_analog_mode;

  Log_InfoPrintf("Joystick %u switched to %s mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  g_host_interface->AddFormattedOSDMessage(
    5.0f,
    m_analog_mode ? g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                    g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
    m_index + 1u);
}

bool AnalogJoystick::Transfer(const u8 data_in, u8* data_out)
{
  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(GetID());
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(GetID() >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = Truncate8(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);

      m_transfer_state = m_analog_mode ? TransferState::RightAxisX : TransferState::Idle;
      return m_analog_mode;
    }

    case TransferState::RightAxisX:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::RightX)]);
      m_transfer_state = TransferState::RightAxisY;
      return true;
    }

    case TransferState::RightAxisY:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::RightY)]);
      m_transfer_state = TransferState::LeftAxisX;
      return true;
    }

    case TransferState::LeftAxisX:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::LeftX)]);
      m_transfer_state = TransferState::LeftAxisY;
      return true;
    }

    case TransferState::LeftAxisY:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::LeftY)]);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<AnalogJoystick> AnalogJoystick::Create(u32 index)
{
  return std::make_unique<AnalogJoystick>(index);
}

std::optional<s32> AnalogJoystick::StaticGetAxisCodeByName(std::string_view axis_name)
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

std::optional<s32> AnalogJoystick::StaticGetButtonCodeByName(std::string_view button_name)
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
  BUTTON(Mode);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList AnalogJoystick::StaticGetAxisNames()
{
  return {{TRANSLATABLE("AnalogJoystick", "LeftX"), static_cast<s32>(Axis::LeftX), AxisType::Full},
          {TRANSLATABLE("AnalogJoystick", "LeftY"), static_cast<s32>(Axis::LeftY), AxisType::Full},
          {TRANSLATABLE("AnalogJoystick", "RightX"), static_cast<s32>(Axis::RightX), AxisType::Full},
          {TRANSLATABLE("AnalogJoystick", "RightY"), static_cast<s32>(Axis::RightY), AxisType::Full}};
}

Controller::ButtonList AnalogJoystick::StaticGetButtonNames()
{
  return {{TRANSLATABLE("AnalogJoystick", "Up"), static_cast<s32>(Button::Up)},
          {TRANSLATABLE("AnalogJoystick", "Down"), static_cast<s32>(Button::Down)},
          {TRANSLATABLE("AnalogJoystick", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("AnalogJoystick", "Right"), static_cast<s32>(Button::Right)},
          {TRANSLATABLE("AnalogJoystick", "Select"), static_cast<s32>(Button::Select)},
          {TRANSLATABLE("AnalogJoystick", "Start"), static_cast<s32>(Button::Start)},
          {TRANSLATABLE("AnalogJoystick", "Triangle"), static_cast<s32>(Button::Triangle)},
          {TRANSLATABLE("AnalogJoystick", "Cross"), static_cast<s32>(Button::Cross)},
          {TRANSLATABLE("AnalogJoystick", "Circle"), static_cast<s32>(Button::Circle)},
          {TRANSLATABLE("AnalogJoystick", "Square"), static_cast<s32>(Button::Square)},
          {TRANSLATABLE("AnalogJoystick", "L1"), static_cast<s32>(Button::L1)},
          {TRANSLATABLE("AnalogJoystick", "L2"), static_cast<s32>(Button::L2)},
          {TRANSLATABLE("AnalogJoystick", "R1"), static_cast<s32>(Button::R1)},
          {TRANSLATABLE("AnalogJoystick", "R2"), static_cast<s32>(Button::R2)},
          {TRANSLATABLE("AnalogJoystick", "L3"), static_cast<s32>(Button::L3)},
          {TRANSLATABLE("AnalogJoystick", "R3"), static_cast<s32>(Button::R3)},
          {TRANSLATABLE("AnalogJoystick", "Analog"), static_cast<s32>(Button::Mode)}};
}

u32 AnalogJoystick::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList AnalogJoystick::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 1> settings = {
    {{SettingInfo::Type::Float, "AxisScale", TRANSLATABLE("AnalogJoystick", "Analog Axis Scale"),
      TRANSLATABLE(
        "AnalogJoystick",
        "Sets the analog stick axis scaling factor. A value between 1.30 and 1.40 is recommended when using recent "
        "controllers, e.g. DualShock 4, Xbox One Controller."),
      "1.00f", "0.01f", "1.50f", "0.01f"}}};

  return SettingList(settings.begin(), settings.end());
}

void AnalogJoystick::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_axis_scale = std::clamp(g_host_interface->GetFloatSettingValue(section, "AxisScale", 1.00f), 0.01f, 1.50f);
}
