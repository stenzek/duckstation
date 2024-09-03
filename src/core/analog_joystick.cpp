// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "analog_joystick.h"
#include "host.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

#include "IconsFontAwesome5.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

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
    Host::AddIconOSDMessage(
      fmt::format("analog_mode_toggle_{}", m_index), ICON_FA_GAMEPAD,
      m_analog_mode ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
                      fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
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

  INFO_LOG("Joystick {} switched to {} mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  Host::AddIconOSDMessage(
    fmt::format("analog_mode_toggle_{}", m_index), ICON_FA_GAMEPAD,
    m_analog_mode ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
                    fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
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
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb                      \
  }
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {                                                                                                                    \
    name, display_name, icon_name, static_cast<u32>(AnalogJoystick::Button::Count) + static_cast<u32>(halfaxis),       \
      InputBindingInfo::Type::HalfAxis, genb                                                                           \
  }

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Up"), ICON_PF_DPAD_UP, AnalogJoystick::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Right"), ICON_PF_DPAD_RIGHT, AnalogJoystick::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Down"), ICON_PF_DPAD_DOWN, AnalogJoystick::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Left"), ICON_PF_DPAD_LEFT, AnalogJoystick::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("AnalogJoystick", "Triangle"), ICON_PF_BUTTON_TRIANGLE, AnalogJoystick::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("AnalogJoystick", "Circle"), ICON_PF_BUTTON_CIRCLE, AnalogJoystick::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("AnalogJoystick", "Cross"), ICON_PF_BUTTON_CROSS, AnalogJoystick::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("AnalogJoystick", "Square"), ICON_PF_BUTTON_SQUARE, AnalogJoystick::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("AnalogJoystick", "Select"), ICON_PF_SELECT_SHARE, AnalogJoystick::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("AnalogJoystick", "Start"), ICON_PF_START, AnalogJoystick::Button::Start, GenericInputBinding::Start),
  BUTTON("Mode", TRANSLATE_NOOP("AnalogJoystick", "Mode Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, AnalogJoystick::Button::Mode, GenericInputBinding::System),
  BUTTON("L1", TRANSLATE_NOOP("AnalogJoystick", "L1"), ICON_PF_LEFT_SHOULDER_L1, AnalogJoystick::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("AnalogJoystick", "R1"), ICON_PF_RIGHT_SHOULDER_R1, AnalogJoystick::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("AnalogJoystick", "L2"), ICON_PF_LEFT_TRIGGER_L2, AnalogJoystick::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("AnalogJoystick", "R2"), ICON_PF_RIGHT_TRIGGER_R2, AnalogJoystick::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", TRANSLATE_NOOP("AnalogJoystick", "L3"), ICON_PF_LEFT_ANALOG_CLICK, AnalogJoystick::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", TRANSLATE_NOOP("AnalogJoystick", "R3"), ICON_PF_RIGHT_ANALOG_CLICK, AnalogJoystick::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Left"), ICON_PF_LEFT_ANALOG_LEFT, AnalogJoystick::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Right"), ICON_PF_LEFT_ANALOG_RIGHT, AnalogJoystick::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Down"), ICON_PF_LEFT_ANALOG_DOWN, AnalogJoystick::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Up"), ICON_PF_LEFT_ANALOG_UP, AnalogJoystick::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Left"), ICON_PF_RIGHT_ANALOG_LEFT, AnalogJoystick::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Right"), ICON_PF_RIGHT_ANALOG_RIGHT, AnalogJoystick::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Down"), ICON_PF_RIGHT_ANALOG_DOWN, AnalogJoystick::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Up"), ICON_PF_RIGHT_ANALOG_UP, AnalogJoystick::HalfAxis::RUp, GenericInputBinding::RightStickUp),
// clang-format on

#undef AXIS
#undef BUTTON
};

static const char* s_invert_settings[] = {TRANSLATE_NOOP("AnalogJoystick", "Not Inverted"),
                                          TRANSLATE_NOOP("AnalogJoystick", "Invert Left/Right"),
                                          TRANSLATE_NOOP("AnalogJoystick", "Invert Up/Down"),
                                          TRANSLATE_NOOP("AnalogJoystick", "Invert Left/Right + Up/Down"), nullptr};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("AnalogJoystick", "Analog Deadzone"),
   TRANSLATE_NOOP("AnalogJoystick",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "1.00f", "0.00f", "1.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("AnalogJoystick", "Analog Sensitivity"),
   TRANSLATE_NOOP(
     "AnalogJoystick",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33f", "0.01f", "2.00f", "0.01f", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::IntegerList, "InvertLeftStick", TRANSLATE_NOOP("AnalogJoystick", "Invert Left Stick"),
   TRANSLATE_NOOP("AnalogJoystick", "Inverts the direction of the left analog stick."), "0", "0", "3", nullptr, nullptr,
   s_invert_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "InvertRightStick", TRANSLATE_NOOP("AnalogJoystick", "Invert Right Stick"),
   TRANSLATE_NOOP("AnalogJoystick", "Inverts the direction of the right analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
};

const Controller::ControllerInfo AnalogJoystick::INFO = {ControllerType::AnalogJoystick,
                                                         "AnalogJoystick",
                                                         TRANSLATE_NOOP("ControllerType", "Analog Joystick"),
                                                         ICON_PF_GAMEPAD,
                                                         s_binding_info,
                                                         s_settings,
                                                         Controller::VibrationCapabilities::NoVibration};

void AnalogJoystick::LoadSettings(SettingsInterface& si, const char* section, bool initial)
{
  Controller::LoadSettings(si, section, initial);
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_invert_left_stick = static_cast<u8>(si.GetIntValue(section, "InvertLeftStick", 0));
  m_invert_right_stick = static_cast<u8>(si.GetIntValue(section, "InvertRightStick", 0));
}
