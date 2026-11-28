// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "analog_joystick.h"
#include "controller_helpers.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/string_util.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

LOG_CHANNEL(Controller);

AnalogJoystick::AnalogJoystick(u32 index) : Controller(index, ControllerType::AnalogJoystick)
{
  m_axis_state.fill(AXIS_CENTER);
  Reset();
}

AnalogJoystick::~AnalogJoystick() = default;

void AnalogJoystick::Reset()
{
  ResetTransferState();
  m_analog_mode = true;
  InputManager::SetPadLEDState(m_index, 1.0f);
}

bool AnalogJoystick::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state))
    return false;

  sw.Do(&m_analog_mode);

  if (sw.IsWriting() || apply_input_state)
  {
    sw.Do(&m_button_state);
    sw.Do(&m_axis_state);
  }
  else
  {
    sw.SkipBytes(sizeof(m_button_state) + sizeof(m_axis_state));
  }

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

  return true;
}

float AnalogJoystick::GetBindState(u32 index) const
{
  if (index >= LED_BIND_START_INDEX)
    return BoolToFloat(index == LED_BIND_START_INDEX && m_analog_mode);
  else if (index >= HALFAXIS_BIND_START_INDEX)
    return static_cast<float>(m_half_axis_state[index - HALFAXIS_BIND_START_INDEX]) * (1.0f / 255.0f);
  else if (index < static_cast<u32>(Button::Mode))
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  else
    return 0.0f;
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
    if (m_half_axis_state[sub_index] == u8_value)
      return;

    m_half_axis_state[sub_index] = u8_value;

    const auto prev_axis_state = m_axis_state;

    // Both axes of the stick are updated, since the deadzone may have centred the other one.
    if (static_cast<HalfAxis>(sub_index) < HalfAxis::RLeft)
    {
      ControllerHelpers::UpdateStickAxes(
        m_half_axis_state[static_cast<size_t>(HalfAxis::LLeft)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::LRight)], m_half_axis_state[static_cast<size_t>(HalfAxis::LUp)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::LDown)], m_invert_left_stick, m_analog_deadzone,
        &m_axis_state[static_cast<u8>(Axis::LeftX)], &m_axis_state[static_cast<u8>(Axis::LeftY)]);
    }
    else
    {
      ControllerHelpers::UpdateStickAxes(
        m_half_axis_state[static_cast<size_t>(HalfAxis::RLeft)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::RRight)], m_half_axis_state[static_cast<size_t>(HalfAxis::RUp)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::RDown)], m_invert_right_stick, m_analog_deadzone,
        &m_axis_state[static_cast<u8>(Axis::RightX)], &m_axis_state[static_cast<u8>(Axis::RightY)]);
    }

    if (std::memcmp(m_axis_state.data(), prev_axis_state.data(), m_axis_state.size()) != 0)
      System::SetRunaheadReplayFlag(true);

    return;
  }

  SetButtonPressed(m_button_state, u16(1) << static_cast<u8>(index), (value >= 0.5f));
}

u32 AnalogJoystick::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> AnalogJoystick::GetAnalogInputBytes() const
{
  return (m_axis_state[static_cast<u8>(Axis::LeftY)] << 24 | m_axis_state[static_cast<u8>(Axis::LeftX)] << 16 |
          m_axis_state[static_cast<u8>(Axis::RightY)] << 8 | m_axis_state[static_cast<u8>(Axis::RightX)]);
}

u8 AnalogJoystick::GetIDByte() const
{
  return m_analog_mode ? 0x53 : 0x41;
}

bool AnalogJoystick::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  // L3/R3 are only sent in analog mode.
  const u16 l3r3_released =
    m_analog_mode ? 0 : static_cast<u16>((1u << static_cast<u8>(Button::L3)) | (1u << static_cast<u8>(Button::R3)));
  const u16 button_state = m_button_state | l3r3_released;
  m_tx_buffer[2] = Truncate8(m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(button_state) : button_state);
  m_tx_buffer[3] = Truncate8(m_button_state >> 8);
  m_tx_buffer[4] = m_axis_state[static_cast<u8>(Axis::RightX)];
  m_tx_buffer[5] = m_axis_state[static_cast<u8>(Axis::RightY)];
  m_tx_buffer[6] = m_axis_state[static_cast<u8>(Axis::LeftX)];
  m_tx_buffer[7] = m_axis_state[static_cast<u8>(Axis::LeftY)];
  return true;
}

void AnalogJoystick::ToggleAnalogMode()
{
  m_analog_mode = !m_analog_mode;

  InputManager::SetPadLEDState(m_index, BoolToFloat(m_analog_mode));

  INFO_LOG("Joystick {} switched to {} mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  Host::AddIconOSDMessage(
    OSDMessageType::Quick, fmt::format("analog_mode_toggle_{}", m_index), ICON_FA_GAMEPAD,
    m_analog_mode ? fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to analog mode."), m_index + 1u) :
                    fmt::format(TRANSLATE_FS("Controller", "Controller {} switched to digital mode."), m_index + 1u));
}

std::unique_ptr<AnalogJoystick> AnalogJoystick::Create(u32 index)
{
  return std::make_unique<AnalogJoystick>(index);
}

constinit const Controller::ControllerBindingInfo AnalogJoystick::s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, image_element, halfaxis, genb)                                             \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   image_element,                                                                                                      \
   HALFAXIS_BIND_START_INDEX + static_cast<u32>(halfaxis),                                                             \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}
#define MODE_LED(name, display_name, icon_name, index, genb)                                                           \
  {name, display_name, icon_name, nullptr, LED_BIND_START_INDEX + index, InputBindingInfo::Type::LED, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Up"), ICON_PF_DPAD_UP, "DPad", AnalogJoystick::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "DPad", AnalogJoystick::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Down"), ICON_PF_DPAD_DOWN, "DPad", AnalogJoystick::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("AnalogJoystick", "D-Pad Left"), ICON_PF_DPAD_LEFT, "DPad", AnalogJoystick::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("AnalogJoystick", "Triangle"), ICON_PF_BUTTON_TRIANGLE, "Triangle", AnalogJoystick::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("AnalogJoystick", "Circle"), ICON_PF_BUTTON_CIRCLE, "Circle", AnalogJoystick::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("AnalogJoystick", "Cross"), ICON_PF_BUTTON_CROSS, "Cross", AnalogJoystick::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("AnalogJoystick", "Square"), ICON_PF_BUTTON_SQUARE, "Square", AnalogJoystick::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("AnalogJoystick", "Select"), ICON_PF_SELECT_SHARE, "Select", AnalogJoystick::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("AnalogJoystick", "Start"), ICON_PF_START, "Start", AnalogJoystick::Button::Start, GenericInputBinding::Start),
  BUTTON("Mode", TRANSLATE_NOOP("AnalogJoystick", "Mode Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, "Mode", AnalogJoystick::Button::Mode, GenericInputBinding::System),
  BUTTON("L1", TRANSLATE_NOOP("AnalogJoystick", "L1"), ICON_PF_LEFT_SHOULDER_L1, "L1", AnalogJoystick::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("AnalogJoystick", "R1"), ICON_PF_RIGHT_SHOULDER_R1, "R1", AnalogJoystick::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("AnalogJoystick", "L2"), ICON_PF_LEFT_TRIGGER_L2, "L2", AnalogJoystick::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("AnalogJoystick", "R2"), ICON_PF_RIGHT_TRIGGER_R2, "R2", AnalogJoystick::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", TRANSLATE_NOOP("AnalogJoystick", "L3"), ICON_PF_LEFT_ANALOG_CLICK, "LStick", AnalogJoystick::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", TRANSLATE_NOOP("AnalogJoystick", "R3"), ICON_PF_RIGHT_ANALOG_CLICK, "RStick", AnalogJoystick::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Left"), ICON_PF_LEFT_ANALOG_LEFT, "LStick", AnalogJoystick::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Right"), ICON_PF_LEFT_ANALOG_RIGHT, "LStick", AnalogJoystick::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Down"), ICON_PF_LEFT_ANALOG_DOWN, "LStick", AnalogJoystick::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", TRANSLATE_NOOP("AnalogJoystick", "Left Stick Up"), ICON_PF_LEFT_ANALOG_UP, "LStick", AnalogJoystick::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Left"), ICON_PF_RIGHT_ANALOG_LEFT, "RStick", AnalogJoystick::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Right"), ICON_PF_RIGHT_ANALOG_RIGHT, "RStick", AnalogJoystick::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Down"), ICON_PF_RIGHT_ANALOG_DOWN, "RStick", AnalogJoystick::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", TRANSLATE_NOOP("AnalogJoystick", "Right Stick Up"), ICON_PF_RIGHT_ANALOG_UP, "RStick", AnalogJoystick::HalfAxis::RUp, GenericInputBinding::RightStickUp),

  MODE_LED("ModeLED", TRANSLATE_NOOP("AnalogJoystick", "Mode LED"), ICON_PF_LED, 0, GenericInputBinding::ModeLED),
// clang-format on

#undef AXIS
#undef BUTTON
#undef MODE_LED
};

static constexpr const char* s_invert_settings[] = {
  TRANSLATE_NOOP("AnalogJoystick", "Not Inverted"), TRANSLATE_NOOP("AnalogJoystick", "Invert Left/Right"),
  TRANSLATE_NOOP("AnalogJoystick", "Invert Up/Down"), TRANSLATE_NOOP("AnalogJoystick", "Invert Left/Right + Up/Down"),
  nullptr};

static constexpr SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "DisableSOCD",
   TRANSLATE_NOOP("AnalogJoystick", "Disable Simultaneous Opposing Cardinal Directions"),
   TRANSLATE_NOOP("AnalogJoystick",
                  "Prevents concurrent left/right or up/down inputs from being presented to the game."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("AnalogJoystick", "Analog Deadzone"),
   TRANSLATE_NOOP("AnalogJoystick",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0", "0", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("AnalogJoystick", "Analog Sensitivity"),
   TRANSLATE_NOOP(
     "AnalogJoystick",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33", "0.01", "2", "0.01", "%.0f%%", nullptr, 100.0f},
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
                                                         ICON_PF_FLIGHT_STICK,
                                                         "images/controllers/analog_joystick.svg",
                                                         s_binding_info,
                                                         s_settings};

void AnalogJoystick::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_invert_left_stick = static_cast<u8>(si.GetIntValue(section, "InvertLeftStick", 0));
  m_invert_right_stick = static_cast<u8>(si.GetIntValue(section, "InvertRightStick", 0));
  m_disable_socd = si.GetBoolValue(section, "DisableSOCD", false);
}
