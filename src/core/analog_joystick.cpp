#include "analog_joystick.h"
#include "common/log.h"
#include "common/string_util.h"
#include "host.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <cmath>
Log_SetChannel(AnalogJoystick);

AnalogJoystick::AnalogJoystick(u32 index) : Controller(index)
{
  m_axis_state.fill(0x80);
  Reset();
}

AnalogJoystick::~AnalogJoystick() = default;

ControllerType AnalogJoystick::GetType() const
{
  return ControllerType::AnalogJoystick;
}

bool AnalogJoystick::InAnalogMode() const
{
  return m_analog_mode;
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
    Host::AddFormattedOSDMessage(5.0f,
                                 m_analog_mode ?
                                   Host::TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                                   Host::TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
                                 m_index + 1u);
  }
  return true;
}

float AnalogJoystick::GetBindState(u32 index) const
{
  if (index >= static_cast<u32>(Button::Count))
  {
    const u32 sub_index = index - static_cast<u32>(Button::Count);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return 0.0f;

    return static_cast<float>(m_half_axis_state[sub_index]) * (1.0f / 255.0f);
  }
  else if (index < static_cast<u32>(Button::Mode))
  {
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  }
  else
  {
    return 0.0f;
  }
}

void AnalogJoystick::SetBindState(u32 index, float value)
{
  if (index == static_cast<s32>(Button::Mode))
  {
    // analog toggle
    if (value >= 0.5f)
      ToggleAnalogMode();

    return;
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    const u32 sub_index = index - static_cast<u32>(Button::Count);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return;

    const u8 u8_value = static_cast<u8>(std::clamp(value * m_analog_sensitivity * 255.0f, 0.0f, 255.0f));
    if (u8_value != m_half_axis_state[sub_index])
      System::SetRunaheadReplayFlag();

    m_half_axis_state[sub_index] = u8_value;

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

  if (value >= 0.5f)
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
  Host::AddFormattedOSDMessage(5.0f,
                               m_analog_mode ?
                                 Host::TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                                 Host::TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
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

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, button, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(button), Controller::ControllerBindingType::Button, genb                      \
  }
#define AXIS(name, display_name, halfaxis, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(AnalogJoystick::Button::Count) + static_cast<u32>(halfaxis),                  \
      Controller::ControllerBindingType::HalfAxis, genb                                                                \
  }

  BUTTON("Up", "D-Pad Up", AnalogJoystick::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", "D-Pad Right", AnalogJoystick::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", "D-Pad Down", AnalogJoystick::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", "D-Pad Left", AnalogJoystick::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", "Triangle", AnalogJoystick::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", "Circle", AnalogJoystick::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", "Cross", AnalogJoystick::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", "Square", AnalogJoystick::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", "Select", AnalogJoystick::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", "Start", AnalogJoystick::Button::Start, GenericInputBinding::Start),
  BUTTON("Mode", "Mode Toggle", AnalogJoystick::Button::Mode, GenericInputBinding::System),
  BUTTON("L1", "L1", AnalogJoystick::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", "R1", AnalogJoystick::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", "L2", AnalogJoystick::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", "R2", AnalogJoystick::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", "L3", AnalogJoystick::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", "R3", AnalogJoystick::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", "Left Stick Left", AnalogJoystick::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", "Left Stick Right", AnalogJoystick::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", "Left Stick Down", AnalogJoystick::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", "Left Stick Up", AnalogJoystick::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", "Right Stick Left", AnalogJoystick::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", "Right Stick Right", AnalogJoystick::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", "Right Stick Down", AnalogJoystick::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", "Right Stick Up", AnalogJoystick::HalfAxis::RUp, GenericInputBinding::RightStickUp),

#undef AXIS
#undef BUTTON
};

static const char* s_invert_settings[] = {TRANSLATABLE("AnalogJoystick", "Not Inverted"),
                                          TRANSLATABLE("AnalogJoystick", "Invert Left/Right"),
                                          TRANSLATABLE("AnalogJoystick", "Invert Up/Down"),
                                          TRANSLATABLE("AnalogJoystick", "Invert Left/Right + Up/Down"), nullptr};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATABLE("AnalogJoystick", "Analog Deadzone"),
   TRANSLATABLE("AnalogJoystick",
                "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "1.00f", "0.00f", "1.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATABLE("AnalogJoystick", "Analog Sensitivity"),
   TRANSLATABLE(
     "AnalogJoystick",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33f", "0.01f", "2.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::IntegerList, "InvertLeftStick", "Invert Left Stick",
   "Inverts the direction of the left analog stick.", "0", "0", "3", nullptr, nullptr, s_invert_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "InvertRightStick", "Invert Right Stick",
   "Inverts the direction of the right analog stick.", "0", "0", "3", nullptr, nullptr, s_invert_settings, 0.0f},
};

const Controller::ControllerInfo AnalogJoystick::INFO = {ControllerType::AnalogJoystick,
                                                         "AnalogJoystick",
                                                         TRANSLATABLE("ControllerType", "Analog Joystick"),
                                                         s_binding_info,
                                                         countof(s_binding_info),
                                                         s_settings,
                                                         countof(s_settings),
                                                         Controller::VibrationCapabilities::NoVibration};

void AnalogJoystick::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_invert_left_stick = static_cast<u8>(si.GetIntValue(section, "InvertLeftStick", 0));
  m_invert_right_stick = static_cast<u8>(si.GetIntValue(section, "InvertRightStick", 0));
}
